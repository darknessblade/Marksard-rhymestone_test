/*
 * This software is experimental and a work in progress.
 * Under no circumstances should these files be used in relation to any critical system(s).
 * Use of these files is at your own risk.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * This files are free to use from http://engsta.com/stm32-flash-memory-eeprom-emulator/ by
 * Artur F.
 *
 * Modifications for QMK and STM32F303 by Yiancar
 * Modifications to add flash wear leveling by Ilya Zhuravlev
 * Modifications to increase flash density by Don Kjer
 * Modifications for QMK and RP2040 by sekigon-gonnoc
 */

#include <stdio.h>
#include <stdbool.h>
#include "util.h"
#include "debug.h"
#include "platforms/pico/flash_pico.h"
#include "hardware/watchdog.h"

/*
 * We emulate eeprom by writing a snapshot compacted view of eeprom contents,
 * followed by a write log of any change since that snapshot:
 *
 * === SIMULATED EEPROM CONTENTS ===
 *
 * ┌─ Compacted ┬ Write Log ─┐
 * │............│[BYTE][BYTE]│
 * │FFFF....FFFF│[WRD0][WRD1]│
 * │FFFFFFFFFFFF│[WORD][NEXT]│
 * │....FFFFFFFF│[BYTE][WRD0]│
 * ├────────────┼────────────┤
 * └──PAGE_BASE │            │
 *    PAGE_LAST─┴─WRITE_BASE │
 *                WRITE_LAST ┘
 *
 * Compacted contents are the 1's complement of the actual EEPROM contents.
 * e.g. An 'FFFF' represents a '0000' value.
 *
 * The size of the 'compacted' area is equal to the size of the 'emulated' eeprom.
 * The size of the compacted-area and write log are configurable, and the combined
 * size of Compacted + WriteLog is a multiple FEE_PAGE_SIZE, which is MCU dependent.
 * Simulated Eeprom contents are located at the end of available flash space.
 *
 * The following configuration defines can be set:
 *
 * FEE_PAGE_COUNT   # Total number of pages to use for eeprom simulation (Compact + Write log)
 * FEE_DENSITY_BYTES   # Size of simulated eeprom. (Defaults to half the space allocated by FEE_PAGE_COUNT)
 * NOTE: The current implementation does not include page swapping,
 * and FEE_DENSITY_BYTES will consume that amount of RAM as a cached view of actual EEPROM contents.
 *
 * The maximum size of FEE_DENSITY_BYTES is currently 16384. The write log size equals
 * FEE_PAGE_COUNT * FEE_PAGE_SIZE - FEE_DENSITY_BYTES.
 * The larger the write log, the less frequently the compacted area needs to be rewritten.
 *
 *
 * *** General Algorithm ***
 *
 * During initialization:
 * The contents of the Compacted-flash area are loaded and the 1's complement value
 * is cached into memory (e.g. 0xFFFF in Flash represents 0x0000 in cache).
 * Write log entries are processed until a 0xFFFF is reached.
 * Each log entry updates a byte or word in the cache.
 *
 * During reads:
 * EEPROM contents are given back directly from the cache in memory.
 *
 * During writes:
 * The contents of the cache is updated first.
 * If the Compacted-flash area corresponding to the write address is unprogrammed, the 1's complement of the value is written directly into Compacted-flash
 * Otherwise:
 * If the write log is full, erase both the Compacted-flash area and the Write log, then write cached contents to the Compacted-flash area.
 * Otherwise a Write log entry is constructed and appended to the next free position in the Write log.
 *
 *
 * *** Write Log Structure ***
 *
 * Write log entries allow for optimized byte writes to addresses below 128. Writing 0 or 1 words are also optimized when word-aligned.
 *
 * === WRITE LOG ENTRY FORMATS ===
 *
 * ╔═══ Byte-Entry ══╗
 * ║0XXXXXXX║YYYYYYYY║
 * ║ └──┬──┘║└──┬───┘║
 * ║ Address║ Value  ║
 * ╚════════╩════════╝
 * 0 <= Address < 0x80 (128)
 *
 * ╔ Word-Encoded 0 ╗
 * ║100XXXXXXXXXXXXX║
 * ║  │└─────┬─────┘║
 * ║  │Address >> 1 ║
 * ║  └── Value: 0  ║
 * ╚════════════════╝
 * 0 <= Address <= 0x3FFE (16382)
 *
 * ╔ Word-Encoded 1 ╗
 * ║101XXXXXXXXXXXXX║
 * ║  │└─────┬─────┘║
 * ║  │Address >> 1 ║
 * ║  └── Value: 1  ║
 * ╚════════════════╝
 * 0 <= Address <= 0x3FFE (16382)
 *
 * ╔═══ Reserved ═══╗
 * ║110XXXXXXXXXXXXX║
 * ╚════════════════╝
 *
 * ╔═══════════ Word-Next ═══════════╗
 * ║111XXXXXXXXXXXXX║YYYYYYYYYYYYYYYY║
 * ║   └─────┬─────┘║└───────┬──────┘║
 * ║(Address-128)>>1║     ~Value     ║
 * ╚════════════════╩════════════════╝
 * (  0 <= Address <  0x0080 (128): Reserved)
 * 0x80 <= Address <= 0x3FFE (16382)
 *
 * Write Log entry ranges:
 * 0x0000 ... 0x7FFF - Byte-Entry;     address is (Entry & 0x7F00) >> 4; value is (Entry & 0xFF)
 * 0x8000 ... 0x9FFF - Word-Encoded 0; address is (Entry & 0x1FFF) << 1; value is 0
 * 0xA000 ... 0xBFFF - Word-Encoded 1; address is (Entry & 0x1FFF) << 1; value is 1
 * 0xC000 ... 0xDFFF - Reserved
 * 0xE000 ... 0xFFBF - Word-Next;      address is (Entry & 0x1FFF) << 1 + 0x80; value is ~(Next_Entry)
 * 0xFFC0 ... 0xFFFE - Reserved
 * 0xFFFF            - Unprogrammed
 *
 */

#include "eeprom_pico_defs.h"
#if !defined(FEE_PAGE_SIZE) || !defined(FEE_PAGE_COUNT) || !defined(FEE_MCU_FLASH_SIZE) || !defined(FEE_PAGE_BASE_ADDRESS)
#    error "not implemented."
#endif

/* These bits are used for optimizing encoding of bytes, 0 and 1 */
#define FEE_WORD_ENCODING 0x8000
#define FEE_VALUE_NEXT 0x6000
#define FEE_VALUE_RESERVED 0x4000
#define FEE_VALUE_ENCODED 0x2000
#define FEE_BYTE_RANGE 0x80

/* Addressable range 16KByte: 0 <-> (0x1FFF << 1) */
#define FEE_ADDRESS_MAX_SIZE 0x4000

/* Flash word value after erase */
#define FEE_EMPTY_WORD ((uint16_t)0xFFFF)

/* Magic number indicating the page is used for FEE */
#define FEE_MAGIC_DWORD ((uint32_t)0x20400FEE)

/* Size of combined compacted eeprom and write log pages */
#define FEE_DENSITY_MAX_SIZE (FEE_PAGE_COUNT * FEE_PAGE_SIZE)

#ifndef FEE_MCU_FLASH_SIZE_IGNORE_CHECK /* *TODO: Get rid of this check */
#    if FEE_DENSITY_MAX_SIZE > (FEE_MCU_FLASH_SIZE * 1024)
#        pragma message STR(FEE_DENSITY_MAX_SIZE) " > " STR(FEE_MCU_FLASH_SIZE * 1024)
#        error emulated eeprom: FEE_DENSITY_MAX_SIZE is greater than available flash size
#    endif
#endif

/* Size of emulated eeprom */
#ifdef FEE_DENSITY_BYTES
#    if (FEE_DENSITY_BYTES > FEE_DENSITY_MAX_SIZE)
#        pragma message STR(FEE_DENSITY_BYTES) " > " STR(FEE_DENSITY_MAX_SIZE)
#        error emulated eeprom: FEE_DENSITY_BYTES exceeds FEE_DENSITY_MAX_SIZE
#    endif
#    if (FEE_DENSITY_BYTES == FEE_DENSITY_MAX_SIZE)
#        pragma message STR(FEE_DENSITY_BYTES) " == " STR(FEE_DENSITY_MAX_SIZE)
#        warning emulated eeprom: FEE_DENSITY_BYTES leaves no room for a write log.  This will greatly increase the flash wear rate!
#    endif
#    if FEE_DENSITY_BYTES > FEE_ADDRESS_MAX_SIZE
#        pragma message STR(FEE_DENSITY_BYTES) " > " STR(FEE_ADDRESS_MAX_SIZE)
#        error emulated eeprom: FEE_DENSITY_BYTES is greater than FEE_ADDRESS_MAX_SIZE allows
#    endif
#    if ((FEE_DENSITY_BYTES) % 2) == 1
#        error emulated eeprom: FEE_DENSITY_BYTES must be even
#    endif
#else
/* Default to half of allocated space used for emulated eeprom, half for write log */
#    define FEE_DENSITY_BYTES (FEE_PAGE_COUNT * FEE_PAGE_SIZE / 2)
#endif

/* Size of write log */
#ifdef FEE_WRITE_LOG_BYTES
#    if ((FEE_DENSITY_BYTES + FEE_WRITE_LOG_BYTES) > FEE_DENSITY_MAX_SIZE)
#        pragma message STR(FEE_DENSITY_BYTES) " + " STR(FEE_WRITE_LOG_BYTES) " > " STR(FEE_DENSITY_MAX_SIZE)
#        error emulated eeprom: FEE_WRITE_LOG_BYTES exceeds remaining FEE_DENSITY_MAX_SIZE
#    endif
#    if ((FEE_WRITE_LOG_BYTES) % 2) == 1
#        error emulated eeprom: FEE_WRITE_LOG_BYTES must be even
#    endif
#else
/* Default to use all remaining space */
#    define FEE_WRITE_LOG_BYTES (FEE_PAGE_COUNT * FEE_PAGE_SIZE - FEE_DENSITY_BYTES)
#endif

/* Start of the emulated eeprom compacted flash area */
#define FEE_COMPACTED_BASE_ADDRESS FEE_PAGE_BASE_ADDRESS
/* End of the emulated eeprom compacted flash area */
#define FEE_COMPACTED_LAST_ADDRESS (FEE_COMPACTED_BASE_ADDRESS + FEE_DENSITY_BYTES)
/* Start of the emulated eeprom write log */
#define FEE_WRITE_LOG_BASE_ADDRESS FEE_COMPACTED_LAST_ADDRESS
/* End of the emulated eeprom write log */
#define FEE_WRITE_LOG_LAST_ADDRESS (FEE_WRITE_LOG_BASE_ADDRESS + FEE_WRITE_LOG_BYTES)

#if defined(DYNAMIC_KEYMAP_EEPROM_MAX_ADDR) && (DYNAMIC_KEYMAP_EEPROM_MAX_ADDR >= FEE_DENSITY_BYTES)
#    error emulated eeprom: DYNAMIC_KEYMAP_EEPROM_MAX_ADDR is greater than the FEE_DENSITY_BYTES available
#endif

/* In-memory contents of emulated eeprom for faster access */
/* *TODO: Implement page swapping */
static uint16_t WordBuf[FEE_DENSITY_BYTES / 2];
static uint8_t *DataBuf = (uint8_t *)WordBuf;

/* Pointer to the first available slot within the write log */
static uint16_t *empty_slot;

static void eeprom_clear(void);

// #define DEBUG_EEPROM_OUTPUT

/*
 * Debug print utils
 */

#if defined(DEBUG_EEPROM_OUTPUT)

#    define debug_eeprom debug_enable
#    define eeprom_println(s) dprintln(s)
#    define eeprom_printf(fmt, ...) dprintf(fmt, ##__VA_ARGS__);

#else /* NO_DEBUG */

#    define debug_eeprom false
#    define eeprom_println(s)
#    define eeprom_printf(fmt, ...)

#endif /* NO_DEBUG */

void print_eeprom(void) {
#ifndef NO_DEBUG
    int empty_rows = 0;
    for (uint16_t i = 0; i < FEE_DENSITY_BYTES; i++) {
        if (i % 16 == 0) {
            if (i >= FEE_DENSITY_BYTES - 16) {
                /* Make sure we display the last row */
                empty_rows = 0;
            }
            /* Check if this row is uninitialized */
            ++empty_rows;
            for (uint16_t j = 0; j < 16; j++) {
                if (DataBuf[i + j]) {
                    empty_rows = 0;
                    break;
                }
            }
            if (empty_rows > 1) {
                /* Repeat empty row */
                if (empty_rows == 2) {
                    /* Only display the first repeat empty row */
                    println("*");
                }
                i += 15;
                continue;
            }
            xprintf("%04x", i);
        }
        if (i % 8 == 0) print(" ");

        xprintf(" %02x", DataBuf[i]);
        if ((i + 1) % 16 == 0) {
            println("");
        }

        watchdog_update();
    }
#endif
}

uint16_t EEPROM_Init(void) {
    /* Load emulated eeprom contents from compacted flash into memory */
    uint16_t *src  = (uint16_t *)FEE_COMPACTED_BASE_ADDRESS;
    uint16_t *dest = (uint16_t *)DataBuf;
    for (; src < (uint16_t *)FEE_COMPACTED_LAST_ADDRESS; ++src, ++dest) {
        *dest = ~*(uint16_t*)((void*)src + XIP_BASE);
    }

    if (debug_eeprom) {
        println("EEPROM_Init Compacted Pages:");
        print_eeprom();
        println("EEPROM_Init Write Log:");
    }

    if (*(uint32_t*)(FEE_WRITE_LOG_BASE_ADDRESS + XIP_BASE) != FEE_MAGIC_DWORD) {
        eeprom_clear();
    }

    /* Replay write log */
    uint16_t *log_addr;
    for (log_addr = (uint16_t *)(FEE_WRITE_LOG_BASE_ADDRESS + 4); log_addr < (uint16_t *)FEE_WRITE_LOG_LAST_ADDRESS; ++log_addr) {

        watchdog_update();

        uint16_t address = *(uint16_t*)((void*)log_addr + XIP_BASE);
        if (address == FEE_EMPTY_WORD) {
            break;
        }
        /* Check for lowest 128-bytes optimization */
        if (!(address & FEE_WORD_ENCODING)) {
            uint8_t bvalue = (uint8_t)address;
            address >>= 8;
            DataBuf[address] = bvalue;
            eeprom_printf("DataBuf[0x%02x] = 0x%02x;\n", address, bvalue);
        } else {
            uint16_t wvalue;
            /* Check if value is in next word */
            if ((address & FEE_VALUE_NEXT) == FEE_VALUE_NEXT) {
                /* Read value from next word */
                if (++log_addr >= (uint16_t *)FEE_WRITE_LOG_LAST_ADDRESS) {
                    break;
                }
                wvalue = ~*(uint16_t *)((void *)log_addr + XIP_BASE);
                if (!wvalue) {
                    eeprom_printf("Incomplete write at log_addr: 0x%04x;\n", (uint32_t)log_addr);
                    /* Possibly incomplete write.  Ignore and continue */
                    continue;
                }
                address &= 0x1FFF;
                address <<= 1;
                /* Writes to addresses less than 128 are byte log entries */
                address += FEE_BYTE_RANGE;
            } else {
                /* Reserved for future use */
                if (address & FEE_VALUE_RESERVED) {
                    eeprom_printf("Reserved encoded value at log_addr: 0x%04x;\n", (uint32_t)log_addr);
                    continue;
                }
                /* Optimization for 0 or 1 values. */
                wvalue = (address & FEE_VALUE_ENCODED) >> 13;
                address &= 0x1FFF;
                address <<= 1;
            }
            if (address < FEE_DENSITY_BYTES) {
                eeprom_printf("DataBuf[0x%04x] = 0x%04x;\n", address, wvalue);
                *(uint16_t *)(&DataBuf[address]) = wvalue;
            } else {
                eeprom_printf("DataBuf[0x%04x] cannot be set to 0x%04x [BAD ADDRESS]\n", address, wvalue);
            }
        }
    }

    empty_slot = log_addr;

    if (debug_eeprom) {
        println("EEPROM_Init Final DataBuf:");
        print_eeprom();
        eeprom_printf("Write Log Usage: %d/%d bytes\n ",
                      (uint32_t)empty_slot - FEE_WRITE_LOG_BASE_ADDRESS,
                      FEE_WRITE_LOG_BYTES);
    }

    return FEE_DENSITY_BYTES;
}

/* Clear flash contents (doesn't touch in-memory DataBuf) */
static void eeprom_clear(void) {
    FLASH_Unlock();

    for (uint16_t page_num = 0; page_num < FEE_PAGE_COUNT; ++page_num) {
        eeprom_printf("FLASH_ErasePage(0x%04x)\n", (uint32_t)(FEE_PAGE_BASE_ADDRESS + (page_num * FEE_PAGE_SIZE)));
        FLASH_ErasePage(FEE_PAGE_BASE_ADDRESS + (page_num * FEE_PAGE_SIZE));
    }

    FLASH_ProgramHalfWord(FEE_WRITE_LOG_BASE_ADDRESS, FEE_MAGIC_DWORD & 0xffff);
    FLASH_ProgramHalfWord(FEE_WRITE_LOG_BASE_ADDRESS + 2,
                          FEE_MAGIC_DWORD >> 16);

    FLASH_Lock();

    empty_slot = (uint16_t *)FEE_WRITE_LOG_BASE_ADDRESS;
    eeprom_printf("eeprom_clear empty_slot: 0x%08x\n", (uint32_t)empty_slot);
}

/* Erase emulated eeprom */
void EEPROM_Erase(void) {
    eeprom_println("EEPROM_Erase");
    /* Erase compacted pages and write log */
    eeprom_clear();
    /* re-initialize to reset DataBuf */
    EEPROM_Init();
}

/* Compact write log */
static uint8_t eeprom_compact(void) {
    /* Erase compacted pages and write log */
    eeprom_clear();

    FLASH_Unlock();

    FLASH_Status final_status = FLASH_COMPLETE;

    /* Write emulated eeprom contents from memory to compacted flash */
    uint16_t *src  = (uint16_t *)DataBuf;
    uintptr_t dest = FEE_COMPACTED_BASE_ADDRESS;
    uint16_t  value;
    for (; dest < FEE_COMPACTED_LAST_ADDRESS; ++src, dest += 2) {
        value = *src;
        if (value) {
            eeprom_printf("FLASH_ProgramHalfWord(0x%04x, 0x%04x)\n", (uint32_t)dest, ~value);
            FLASH_Status status = FLASH_ProgramHalfWord(dest, ~value);
            if (status != FLASH_COMPLETE) final_status = status;
        }
    }

    FLASH_Lock();

    if (debug_eeprom) {
        println("eeprom_compacted:");
        print_eeprom();
    }

    return final_status;
}

static uint8_t eeprom_write_direct_entry(uint16_t Address) {
    /* Check if we can just write this directly to the compacted flash area */
    uintptr_t directAddress = FEE_COMPACTED_BASE_ADDRESS + (Address & 0xFFFE);
    if (*(uint16_t *)((void *)directAddress + XIP_BASE) == FEE_EMPTY_WORD) {
        /* Write the value directly to the compacted area without a log entry */
        uint16_t value = ~*(uint16_t *)(&DataBuf[Address & 0xFFFE]);
        /* Early exit if a write isn't needed */
        if (value == FEE_EMPTY_WORD) return FLASH_COMPLETE;

        FLASH_Unlock();

        eeprom_printf("FLASH_ProgramHalfWord(0x%08x, 0x%04x) [DIRECT]\n", (uint32_t)directAddress, value);
        FLASH_Status status = FLASH_ProgramHalfWord(directAddress, value);

        FLASH_Lock();
        return status;
    }
    return 0;
}

static uint8_t eeprom_write_log_word_entry(uint16_t Address) {
    FLASH_Status final_status = FLASH_COMPLETE;

    uint16_t value = *(uint16_t *)(&DataBuf[Address]);
    eeprom_printf("eeprom_write_log_word_entry(0x%04x): 0x%04x\n", Address, value);

    /* MSB signifies the lowest 128-byte optimization is not in effect */
    uint16_t encoding = FEE_WORD_ENCODING;
    uint8_t  entry_size;
    if (value <= 1) {
        encoding |= value << 13;
        entry_size = 2;
    } else {
        encoding |= FEE_VALUE_NEXT;
        entry_size = 4;
        /* Writes to addresses less than 128 are byte log entries */
        Address -= FEE_BYTE_RANGE;
    }

    /* if we can't find an empty spot, we must compact emulated eeprom */
    if (empty_slot > (uint16_t *)(FEE_WRITE_LOG_LAST_ADDRESS - entry_size)) {
        /* compact the write log into the compacted flash area */
        return eeprom_compact();
    }

    /* Word log writes should be word-aligned.  Take back a bit */
    Address >>= 1;
    Address |= encoding;

    /* ok we found a place let's write our data */
    FLASH_Unlock();

    /* address */
    eeprom_printf("FLASH_ProgramHalfWord(0x%08x, 0x%04x)\n", (uint32_t)empty_slot, Address);
    final_status = FLASH_ProgramHalfWord((uintptr_t)empty_slot++, Address);

    /* value */
    if (encoding == (FEE_WORD_ENCODING | FEE_VALUE_NEXT)) {
        eeprom_printf("FLASH_ProgramHalfWord(0x%08x, 0x%04x)\n", (uint32_t)empty_slot, ~value);
        FLASH_Status status = FLASH_ProgramHalfWord((uintptr_t)empty_slot++, ~value);
        if (status != FLASH_COMPLETE) final_status = status;
    }

    FLASH_Lock();

    return final_status;
}

static uint8_t eeprom_write_log_byte_entry(uint16_t Address) {
    eeprom_printf("eeprom_write_log_byte_entry(0x%04x): 0x%02x\n", Address, DataBuf[Address]);

    /* if couldn't find an empty spot, we must compact emulated eeprom */
    if (empty_slot >= (uint16_t *)FEE_WRITE_LOG_LAST_ADDRESS) {
        /* compact the write log into the compacted flash area */
        return eeprom_compact();
    }

    /* ok we found a place let's write our data */
    FLASH_Unlock();

    /* Pack address and value into the same word */
    uint16_t value = (Address << 8) | DataBuf[Address];

    /* write to flash */
    eeprom_printf("FLASH_ProgramHalfWord(0x%08x, 0x%04x)\n", (uint32_t)empty_slot, value);
    FLASH_Status status = FLASH_ProgramHalfWord((uintptr_t)empty_slot++, value);

    FLASH_Lock();

    return status;
}

uint8_t EEPROM_WriteDataByte(uint16_t Address, uint8_t DataByte) {
    /* if the address is out-of-bounds, do nothing */
    if (Address >= FEE_DENSITY_BYTES) {
        eeprom_printf("EEPROM_WriteDataByte(0x%04x, 0x%02x) [BAD ADDRESS]\n", Address, DataByte);
        return FLASH_BAD_ADDRESS;
    }

    /* if the value is the same, don't bother writing it */
    if (DataBuf[Address] == DataByte) {
        eeprom_printf("EEPROM_WriteDataByte(0x%04x, 0x%02x) [SKIP SAME]\n", Address, DataByte);
        return 0;
    }

    /* keep DataBuf cache in sync */
    DataBuf[Address] = DataByte;
    eeprom_printf("EEPROM_WriteDataByte DataBuf[0x%04x] = 0x%02x\n", Address, DataBuf[Address]);

    /* perform the write into flash memory */
    /* First, attempt to write directly into the compacted flash area */
    FLASH_Status status = eeprom_write_direct_entry(Address);
    if (!status) {
        /* Otherwise append to the write log */
        if (Address < FEE_BYTE_RANGE) {
            status = eeprom_write_log_byte_entry(Address);
        } else {
            status = eeprom_write_log_word_entry(Address & 0xFFFE);
        }
    }
    if (status != 0 && status != FLASH_COMPLETE) {
        eeprom_printf("EEPROM_WriteDataByte [STATUS == %d]\n", status);
    }
    return status;
}

uint8_t EEPROM_WriteDataWord(uint16_t Address, uint16_t DataWord) {
    /* if the address is out-of-bounds, do nothing */
    if (Address >= FEE_DENSITY_BYTES) {
        eeprom_printf("EEPROM_WriteDataWord(0x%04x, 0x%04x) [BAD ADDRESS]\n", Address, DataWord);
        return FLASH_BAD_ADDRESS;
    }

    /* Check for word alignment */
    FLASH_Status final_status = FLASH_COMPLETE;
    if (Address % 2) {
        final_status        = EEPROM_WriteDataByte(Address, DataWord);
        FLASH_Status status = EEPROM_WriteDataByte(Address + 1, DataWord >> 8);
        if (status != FLASH_COMPLETE) final_status = status;
        if (final_status != 0 && final_status != FLASH_COMPLETE) {
            eeprom_printf("EEPROM_WriteDataWord [STATUS == %d]\n", final_status);
        }
        return final_status;
    }

    /* if the value is the same, don't bother writing it */
    uint16_t oldValue = *(uint16_t *)(&DataBuf[Address]);
    if (oldValue == DataWord) {
        eeprom_printf("EEPROM_WriteDataWord(0x%04x, 0x%04x) [SKIP SAME]\n", Address, DataWord);
        return 0;
    }

    /* keep DataBuf cache in sync */
    *(uint16_t *)(&DataBuf[Address]) = DataWord;
    eeprom_printf("EEPROM_WriteDataWord DataBuf[0x%04x] = 0x%04x\n", Address, *(uint16_t *)(&DataBuf[Address]));

    /* perform the write into flash memory */
    /* First, attempt to write directly into the compacted flash area */
    final_status = eeprom_write_direct_entry(Address);
    if (!final_status) {
        /* Otherwise append to the write log */
        /* Check if we need to fall back to byte write */
        if (Address < FEE_BYTE_RANGE) {
            final_status = FLASH_COMPLETE;
            /* Only write a byte if it has changed */
            if ((uint8_t)oldValue != (uint8_t)DataWord) {
                final_status = eeprom_write_log_byte_entry(Address);
            }
            FLASH_Status status = FLASH_COMPLETE;
            /* Only write a byte if it has changed */
            if ((oldValue >> 8) != (DataWord >> 8)) {
                status = eeprom_write_log_byte_entry(Address + 1);
            }
            if (status != FLASH_COMPLETE) final_status = status;
        } else {
            final_status = eeprom_write_log_word_entry(Address);
        }
    }
    if (final_status != 0 && final_status != FLASH_COMPLETE) {
        eeprom_printf("EEPROM_WriteDataWord [STATUS == %d]\n", final_status);
    }
    return final_status;
}

uint8_t EEPROM_ReadDataByte(uint16_t Address) {
    uint8_t DataByte = 0xFF;

    if (Address < FEE_DENSITY_BYTES) {
        DataByte = DataBuf[Address];
    }

    eeprom_printf("EEPROM_ReadDataByte(0x%04x): 0x%02x\n", Address, DataByte);

    return DataByte;
}

uint16_t EEPROM_ReadDataWord(uint16_t Address) {
    uint16_t DataWord = 0xFFFF;

    if (Address < FEE_DENSITY_BYTES - 1) {
        /* Check word alignment */
        if (Address % 2) {
            DataWord = DataBuf[Address] | (DataBuf[Address + 1] << 8);
        } else {
            DataWord = *(uint16_t *)(&DataBuf[Address]);
        }
    }

    eeprom_printf("EEPROM_ReadDataWord(0x%04x): 0x%04x\n", Address, DataWord);

    return DataWord;
}

/*****************************************************************************
 *  Wrap library in AVR style functions.
 *******************************************************************************/
uint8_t eeprom_read_byte(const uint8_t *Address) { return EEPROM_ReadDataByte((const uintptr_t)Address); }

void eeprom_write_byte(uint8_t *Address, uint8_t Value) { EEPROM_WriteDataByte((uintptr_t)Address, Value); }

void eeprom_update_byte(uint8_t *Address, uint8_t Value) { EEPROM_WriteDataByte((uintptr_t)Address, Value); }

uint16_t eeprom_read_word(const uint16_t *Address) { return EEPROM_ReadDataWord((const uintptr_t)Address); }

void eeprom_write_word(uint16_t *Address, uint16_t Value) { EEPROM_WriteDataWord((uintptr_t)Address, Value); }

void eeprom_update_word(uint16_t *Address, uint16_t Value) { EEPROM_WriteDataWord((uintptr_t)Address, Value); }

uint32_t eeprom_read_dword(const uint32_t *Address) {
    const uint16_t p = (const uintptr_t)Address;
    /* Check word alignment */
    if (p % 2) {
        /* Not aligned */
        return (uint32_t)EEPROM_ReadDataByte(p) | (uint32_t)(EEPROM_ReadDataWord(p + 1) << 8) | (uint32_t)(EEPROM_ReadDataByte(p + 3) << 24);
    } else {
        /* Aligned */
        return EEPROM_ReadDataWord(p) | (EEPROM_ReadDataWord(p + 2) << 16);
    }
}

void eeprom_write_dword(uint32_t *Address, uint32_t Value) {
    uint16_t p = (const uintptr_t)Address;
    /* Check word alignment */
    if (p % 2) {
        /* Not aligned */
        EEPROM_WriteDataByte(p, (uint8_t)Value);
        EEPROM_WriteDataWord(p + 1, (uint16_t)(Value >> 8));
        EEPROM_WriteDataByte(p + 3, (uint8_t)(Value >> 24));
    } else {
        /* Aligned */
        EEPROM_WriteDataWord(p, (uint16_t)Value);
        EEPROM_WriteDataWord(p + 2, (uint16_t)(Value >> 16));
    }
}

void eeprom_update_dword(uint32_t *Address, uint32_t Value) { eeprom_write_dword(Address, Value); }

void eeprom_read_block(void *buf, const void *addr, size_t len) {
    const uint8_t *src  = (const uint8_t *)addr;
    uint8_t *      dest = (uint8_t *)buf;

    /* Check word alignment */
    if (len && (uintptr_t)src % 2) {
        /* Read the unaligned first byte */
        *dest++ = eeprom_read_byte(src++);
        --len;
    }

    uint16_t value;
    bool     aligned = ((uintptr_t)dest % 2 == 0);
    while (len > 1) {
        value = eeprom_read_word((uint16_t *)src);
        if (aligned) {
            *(uint16_t *)dest = value;
            dest += 2;
        } else {
            *dest++ = value;
            *dest++ = value >> 8;
        }
        src += 2;
        len -= 2;
    }
    if (len) {
        *dest = eeprom_read_byte(src);
    }
}

void eeprom_write_block(const void *buf, void *addr, size_t len) {
    uint8_t *      dest = (uint8_t *)addr;
    const uint8_t *src  = (const uint8_t *)buf;

    /* Check word alignment */
    if (len && (uintptr_t)dest % 2) {
        /* Write the unaligned first byte */
        eeprom_write_byte(dest++, *src++);
        --len;
    }

    uint16_t value;
    bool     aligned = ((uintptr_t)src % 2 == 0);
    while (len > 1) {
        if (aligned) {
            value = *(uint16_t *)src;
        } else {
            value = *(uint8_t *)src | (*(uint8_t *)(src + 1) << 8);
        }
        eeprom_write_word((uint16_t *)dest, value);
        dest += 2;
        src += 2;
        len -= 2;
    }

    if (len) {
        eeprom_write_byte(dest, *src);
    }
}

void eeprom_update_block(const void *buf, void *addr, size_t len) { eeprom_write_block(buf, addr, len); }
