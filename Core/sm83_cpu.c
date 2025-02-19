#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include "gb.h"
#include "libRetroReversing/include/libRR_c.h"

const char *get_src_name(uint8_t opcode);
static char *register_names[] = {"af", "bc", "de", "hl", "sp"};
static char *high_register_names[] = {"a", "b", "d", "h", "s"};
static char *low_register_names[] = {"f", "c", "e", "l", "p"};

const char *get_condition_code_name(uint8_t opcode)
{
    switch ((opcode >> 3) & 0x3) {
        case 0:
            return "nz";
        case 1:
            return "z";
        case 2:
            return "nc";
        case 3:
            return "c";
    }

    return NULL;
}

typedef void GB_opcode_t(GB_gameboy_t *gb, uint8_t opcode);

typedef enum {
    /* Default behavior. If the CPU writes while another component reads, it reads the old value */
    GB_CONFLICT_READ_OLD,
    /* If the CPU writes while another component reads, it reads the new value */
    GB_CONFLICT_READ_NEW,
    /* If the CPU and another component write at the same time, the CPU's value "wins" */
    GB_CONFLICT_WRITE_CPU,
    /* Register specific values */
    GB_CONFLICT_STAT_CGB,
    GB_CONFLICT_STAT_DMG,
    GB_CONFLICT_PALETTE_DMG,
    GB_CONFLICT_PALETTE_CGB,
    GB_CONFLICT_DMG_LCDC,
    GB_CONFLICT_SGB_LCDC,
    GB_CONFLICT_WX,
} GB_conflict_t;

/* Todo: How does double speed mode affect these? */
static const GB_conflict_t cgb_conflict_map[0x80] = {
    [GB_IO_IF] = GB_CONFLICT_WRITE_CPU,
    [GB_IO_LYC] = GB_CONFLICT_WRITE_CPU,
    [GB_IO_STAT] = GB_CONFLICT_STAT_CGB,
    [GB_IO_BGP] = GB_CONFLICT_PALETTE_CGB,
    [GB_IO_OBP0] = GB_CONFLICT_PALETTE_CGB,
    [GB_IO_OBP1] = GB_CONFLICT_PALETTE_CGB,
    
    /* Todo: most values not verified, and probably differ between revisions */
};

/* Todo: verify on an MGB */
static const GB_conflict_t dmg_conflict_map[0x80] = {
    [GB_IO_IF] = GB_CONFLICT_WRITE_CPU,
    [GB_IO_LYC] = GB_CONFLICT_READ_OLD,
    [GB_IO_LCDC] = GB_CONFLICT_DMG_LCDC,
    [GB_IO_SCY] = GB_CONFLICT_READ_NEW,
    [GB_IO_STAT] = GB_CONFLICT_STAT_DMG,
 
    [GB_IO_BGP] = GB_CONFLICT_PALETTE_DMG,
    [GB_IO_OBP0] = GB_CONFLICT_PALETTE_DMG,
    [GB_IO_OBP1] = GB_CONFLICT_PALETTE_DMG,
    [GB_IO_WY] = GB_CONFLICT_READ_OLD,
    [GB_IO_WX] = GB_CONFLICT_WX,

    /* Todo: these were not verified at all */
    [GB_IO_SCX] = GB_CONFLICT_READ_NEW,
};

/* Todo: Verify on an SGB1 */
static const GB_conflict_t sgb_conflict_map[0x80] = {
    [GB_IO_IF] = GB_CONFLICT_WRITE_CPU,
    [GB_IO_LYC] = GB_CONFLICT_READ_OLD,
    [GB_IO_LCDC] = GB_CONFLICT_SGB_LCDC,
    [GB_IO_SCY] = GB_CONFLICT_READ_NEW,
    [GB_IO_STAT] = GB_CONFLICT_STAT_DMG,
    
    [GB_IO_BGP] = GB_CONFLICT_READ_NEW,
    [GB_IO_OBP0] = GB_CONFLICT_READ_NEW,
    [GB_IO_OBP1] = GB_CONFLICT_READ_NEW,
    [GB_IO_WY] = GB_CONFLICT_READ_OLD,
    [GB_IO_WX] = GB_CONFLICT_WX,

    /* Todo: these were not verified at all */
    [GB_IO_SCX] = GB_CONFLICT_READ_NEW,
};

static uint8_t cycle_read(GB_gameboy_t *gb, uint16_t addr)
{
    if (gb->pending_cycles) {
        GB_advance_cycles(gb, gb->pending_cycles);
    }
    uint8_t ret = GB_read_memory(gb, addr);
    gb->pending_cycles = 4;
    return ret;
}

static uint8_t cycle_read_data_only(GB_gameboy_t *gb, uint16_t addr, const char* type, uint8_t byte_size) {
    uint8_t value = cycle_read(gb, addr);
    if (libRR_full_function_log) {
        char bytes[byte_size+1];
        bytes[0] = value;
        for (int i=1; i<=byte_size; i++) {
            bytes[i] = cycle_read(gb, addr+i);
        }
        libRR_gameboy_log_memory_read(addr, type, byte_size, bytes);
    }
    return value;
}

static uint8_t cycle_read_inc_oam_bug(GB_gameboy_t *gb, uint16_t addr)
{
    if (gb->pending_cycles) {
        GB_advance_cycles(gb, gb->pending_cycles);
    }
    GB_trigger_oam_bug_read_increase(gb, addr); /* Todo: test T-cycle timing */
    uint8_t ret = GB_read_memory(gb, addr);
    gb->pending_cycles = 4;
    return ret;
}

// This is a data only version used for logging only data accesses to the rom not instruction accesses
static uint8_t cycle_read_inc_oam_bug_data_only(GB_gameboy_t *gb, uint16_t addr, const char* type, uint8_t byte_size) {
    if (libRR_full_function_log) {
        cycle_read_data_only(gb, addr, type, byte_size);
    }
    return cycle_read_inc_oam_bug(gb, addr);
}

/* A special case for IF during ISR, returns the old value of IF. */
/* TODO: Verify the timing, it might be wrong in cases where, in the same M cycle, IF
   is both read be the CPU, modified by the ISR, and modified by an actual interrupt.
   If this timing proves incorrect, the ISR emulation must be updated so IF reads are
   timed correctly. */
static uint8_t cycle_write_if(GB_gameboy_t *gb, uint8_t value)
{
    assert(gb->pending_cycles);
    GB_advance_cycles(gb, gb->pending_cycles);
    uint8_t old = (gb->io_registers[GB_IO_IF]) & 0x1F;
    GB_write_memory(gb, 0xFF00 + GB_IO_IF, value);
    gb->pending_cycles = 4;
    return old;
}

static void cycle_write(GB_gameboy_t *gb, uint16_t addr, uint8_t value)
{
    assert(gb->pending_cycles);
    GB_conflict_t conflict = GB_CONFLICT_READ_OLD;
    if ((addr & 0xFF80) == 0xFF00) {
        const GB_conflict_t *map = NULL;
        if (GB_is_cgb(gb)) {
            map = cgb_conflict_map;
        }
        else if (GB_is_sgb(gb)) {
            map = sgb_conflict_map;
        }
        else {
            map = dmg_conflict_map;
        }
        conflict = map[addr & 0x7F];
    }
    switch (conflict) {
        case GB_CONFLICT_READ_OLD:
            GB_advance_cycles(gb, gb->pending_cycles);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 4;
            return;
            
        case GB_CONFLICT_READ_NEW:
            GB_advance_cycles(gb, gb->pending_cycles - 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 5;
            return;
            
        case GB_CONFLICT_WRITE_CPU:
            GB_advance_cycles(gb, gb->pending_cycles + 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 3;
            return;
        
        /* The DMG STAT-write bug is basically the STAT register being read as FF for a single T-cycle */
        case GB_CONFLICT_STAT_DMG:
            GB_advance_cycles(gb, gb->pending_cycles);
            /* State 7 is the edge between HBlank and OAM mode, and it behaves a bit weird.
             The OAM interrupt seems to be blocked by HBlank interrupts in that case, despite
             the timing not making much sense for that.
             This is a hack to simulate this effect */
            if (gb->display_state == 7 && (gb->io_registers[GB_IO_STAT] & 0x28) == 0x08) {
                GB_write_memory(gb, addr, ~0x20);
            }
            else {
                GB_write_memory(gb, addr, 0xFF);
            }
            GB_advance_cycles(gb, 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 3;
            return;
        
        case GB_CONFLICT_STAT_CGB: {
            /* Todo: Verify this with SCX adjustments */
            /* The LYC bit behaves differently */
            uint8_t old_value = GB_read_memory(gb, addr);
            GB_advance_cycles(gb, gb->pending_cycles);
            GB_write_memory(gb, addr, (old_value & 0x40) | (value & ~0x40));
            GB_advance_cycles(gb, 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 3;
            return;
        }
        
        /* There is some "time travel" going on with these two values, as it appears
           that there's some off-by-1-T-cycle timing issue in the PPU implementation.
         
           This is should be accurate for every measureable scenario, though. */
            
        case GB_CONFLICT_PALETTE_DMG: {
            GB_advance_cycles(gb, gb->pending_cycles - 2);
            uint8_t old_value = GB_read_memory(gb, addr);
            GB_write_memory(gb, addr, value | old_value);
            GB_advance_cycles(gb, 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 5;
            return;
        }
            
        case GB_CONFLICT_PALETTE_CGB: {
            GB_advance_cycles(gb, gb->pending_cycles - 2);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 6;
            return;
        }
            
        case GB_CONFLICT_DMG_LCDC: {
            /* Similar to the palette registers, these interact directly with the LCD, so they appear to be affected by it. Both my DMG (B, blob) and Game Boy Light behave this way though.
             
               Additionally, LCDC.1 is very nasty because on the it is read both by the FIFO when popping pixels,
               and the sprite-fetching state machine, and both behave differently when it comes to access conflicts.
               Hacks ahead.
             */
            
            
            
            uint8_t old_value = GB_read_memory(gb, addr);
            GB_advance_cycles(gb, gb->pending_cycles - 2);
            
            if (/* gb->model != GB_MODEL_MGB && */ gb->position_in_line == 0 && (old_value & 2) && !(value & 2)) {
                old_value &= ~2;
            }
            
            GB_write_memory(gb, addr, old_value | (value & 1));
            GB_advance_cycles(gb, 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 5;
            return;
        }
            
        case GB_CONFLICT_SGB_LCDC: {
            /* Simplified version of the above */
            
            uint8_t old_value = GB_read_memory(gb, addr);
            GB_advance_cycles(gb, gb->pending_cycles - 2);
            /* Hack to force aborting object fetch */
            GB_write_memory(gb, addr, value);
            GB_write_memory(gb, addr, old_value);
            GB_advance_cycles(gb, 1);
            GB_write_memory(gb, addr, value);
            gb->pending_cycles = 5;
            return;
        }
            
        case GB_CONFLICT_WX:
            GB_advance_cycles(gb, gb->pending_cycles);
            GB_write_memory(gb, addr, value);
            gb->wx_just_changed = true;
            GB_advance_cycles(gb, 1);
            gb->wx_just_changed = false;
            gb->pending_cycles = 3;
            return;
    }
}

static void cycle_no_access(GB_gameboy_t *gb)
{
    gb->pending_cycles += 4;
}

static void cycle_oam_bug(GB_gameboy_t *gb, uint8_t register_id)
{
    if (GB_is_cgb(gb)) {
        /* Slight optimization */
        gb->pending_cycles += 4;
        return;
    }
    if (gb->pending_cycles) {
        GB_advance_cycles(gb, gb->pending_cycles);
    }
    GB_trigger_oam_bug(gb, gb->registers[register_id]); /* Todo: test T-cycle timing */
    gb->pending_cycles = 4;

}

static void flush_pending_cycles(GB_gameboy_t *gb)
{
    if (gb->pending_cycles) {
        GB_advance_cycles(gb, gb->pending_cycles);
    }
    gb->pending_cycles = 0;
}

/* Todo: test if multi-byte opcodes trigger the OAM bug correctly */

static void ill(GB_gameboy_t *gb, uint8_t opcode)
{
    GB_log(gb, "Illegal Opcode. Halting.\n");
    gb->interrupt_enable = 0;
    gb->halted = true;
}

static void nop(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "nop", opcode, 1);
}

static void enter_stop_mode(GB_gameboy_t *gb)
{
    gb->stopped = true;
    gb->oam_ppu_blocked = !gb->oam_read_blocked;
    gb->vram_ppu_blocked = !gb->vram_read_blocked;
    gb->cgb_palettes_ppu_blocked = !gb->cgb_palettes_blocked;
}

static void leave_stop_mode(GB_gameboy_t *gb)
{
    /* The CPU takes more time to wake up then the other components */
    for (unsigned i = 0x200; i--;) {
        GB_advance_cycles(gb, 0x10);
    }
    gb->stopped = false;
    gb->oam_ppu_blocked = false;
    gb->vram_ppu_blocked = false;
    gb->cgb_palettes_ppu_blocked = false;
}

static void stop(GB_gameboy_t *gb, uint8_t opcode)
{
    if (gb->io_registers[GB_IO_KEY1] & 0x1) {
        flush_pending_cycles(gb);
        bool needs_alignment = false;
        
        GB_advance_cycles(gb, 0x4);
        /* Make sure we keep the CPU ticks aligned correctly when returning from double speed mode */
        if (gb->double_speed_alignment & 7) {
            GB_advance_cycles(gb, 0x4);
            needs_alignment = true;
        }

        gb->cgb_double_speed ^= true;
        gb->io_registers[GB_IO_KEY1] = 0;
        
        enter_stop_mode(gb);
        leave_stop_mode(gb);
        
        if (!needs_alignment) {
            GB_advance_cycles(gb, 0x4);
        }
        
    }
    else {
        GB_timing_sync(gb);
        if ((gb->io_registers[GB_IO_JOYP] & 0xF) != 0xF) {
            /* HW Bug? When STOP is executed while a button is down, the CPU halts forever
               yet the other hardware keeps running. */
            gb->interrupt_enable = 0;
            gb->halted = true;
        }
        else {
            enter_stop_mode(gb);
        }
    }
    
    /* Todo: is PC being actually read? */
    gb->pc++;
}

/* Operand naming conventions for functions:
   r = 8-bit register
   lr = low 8-bit register
   hr = high 8-bit register
   rr = 16-bit register
   d8 = 8-bit imm
   d16 = 16-bit imm
   d.. = [..]
   cc = condition code (z, nz, c, nc)
   */

static void ld_rr_d16(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id, value2;
    uint16_t value;
    register_id = (opcode >> 4) + 1;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);
    value2 = cycle_read_inc_oam_bug(gb, gb->pc);
    
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        uint32_t b2b1 = three_bytes_to_24bit_value(value2, (uint8_t)value, opcode);
        int16_t both_operands = value;
        both_operands |= value2 << 8; //two_bytes_to_16bit_value(value2, (uint8_t)value);
        libRR_log_instruction_1int_registername(current_pc, "ld %r%, %int%", b2b1, 3, both_operands, register_names[register_id]);
    }
    
    // value |= value2;
    value |= cycle_read_inc_oam_bug(gb, gb->pc++) << 8;
    gb->registers[register_id] = value;
}

static void ld_drr_a(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "ld_drr_a", opcode, 1);
    uint8_t register_id;
    register_id = (opcode >> 4) + 1;
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "ld [%r%], a", opcode, 1, opcode, register_names[register_id]);
    }
    cycle_write(gb, gb->registers[register_id], gb->registers[GB_REGISTER_AF] >> 8);
}

static void inc_rr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id = (opcode >> 4) + 1;

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "inc %r%", opcode, 1, opcode, register_names[register_id]);
    }

    cycle_oam_bug(gb, register_id);
    gb->registers[register_id]++;
}

static void inc_hr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    register_id = ((opcode >> 4) + 1) & 0x03;


    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "inc %r%", opcode, 1, opcode, high_register_names[register_id]);
    }

    gb->registers[register_id] += 0x100;
    gb->registers[GB_REGISTER_AF] &= ~(GB_SUBTRACT_FLAG | GB_ZERO_FLAG | GB_HALF_CARRY_FLAG);

    if ((gb->registers[register_id] & 0x0F00) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((gb->registers[register_id] & 0xFF00) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}
static void dec_hr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    register_id = ((opcode >> 4) + 1) & 0x03;

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "dec %r%", opcode, 1, opcode, high_register_names[register_id]);
    }

    gb->registers[register_id] -= 0x100;
    gb->registers[GB_REGISTER_AF] &= ~(GB_ZERO_FLAG | GB_HALF_CARRY_FLAG);
    gb->registers[GB_REGISTER_AF] |= GB_SUBTRACT_FLAG;

    if ((gb->registers[register_id] & 0x0F00) == 0xF00) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((gb->registers[register_id] & 0xFF00) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void ld_hr_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    register_id = ((opcode >> 4) + 1) & 0x03;

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        uint8_t value = cycle_read_inc_oam_bug(gb, gb->pc);
        uint32_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int_registername(current_pc, "ld %r%, %int%", b2b1, 2, value, high_register_names[register_id]);
    }

    gb->registers[register_id] &= 0xFF;
    gb->registers[register_id] |= cycle_read_inc_oam_bug(gb, gb->pc++) << 8;
}

static void rlca(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "RLCA", opcode, 1);
    bool carry = (gb->registers[GB_REGISTER_AF] & 0x8000) != 0;

    gb->registers[GB_REGISTER_AF] = (gb->registers[GB_REGISTER_AF] & 0xFF00) << 1;
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG | 0x0100;
    }
}

static void rla(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "RLA", opcode, 1);
    bool bit7 = (gb->registers[GB_REGISTER_AF] & 0x8000) != 0;
    bool carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;

    gb->registers[GB_REGISTER_AF] = (gb->registers[GB_REGISTER_AF] & 0xFF00) << 1;
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= 0x0100;
    }
    if (bit7) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void ld_da16_sp(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ld_da16_sp", opcode, 2);
    /* Todo: Verify order is correct */
    uint16_t addr;
    addr = cycle_read_inc_oam_bug(gb, gb->pc++);
    addr |= cycle_read_inc_oam_bug(gb, gb->pc++) << 8;
    cycle_write(gb, addr, gb->registers[GB_REGISTER_SP] & 0xFF);
    cycle_write(gb, addr + 1, gb->registers[GB_REGISTER_SP] >> 8);
}

static void add_hl_rr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint16_t hl = gb->registers[GB_REGISTER_HL];
    uint16_t rr;
    uint8_t register_id;
    cycle_no_access(gb);
    register_id = (opcode >> 4) + 1;

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "add hl, %r%", opcode, 1, opcode, register_names[register_id]);
    }

    rr = gb->registers[register_id];
    gb->registers[GB_REGISTER_HL] = hl + rr;
    gb->registers[GB_REGISTER_AF] &= ~(GB_SUBTRACT_FLAG | GB_CARRY_FLAG | GB_HALF_CARRY_FLAG);

    /* The meaning of the Half Carry flag is really hard to track -_- */
    if (((hl & 0xFFF) + (rr & 0xFFF)) & 0x1000) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ( ((unsigned) hl + (unsigned) rr) & 0x10000) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void ld_a_drr(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "ld_a_drr", opcode, 1);
    uint8_t register_id;
    register_id = (opcode >> 4) + 1;

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "ld a, [%r%]", opcode, 1, opcode, register_names[register_id]);
    }

    gb->registers[GB_REGISTER_AF] &= 0xFF;
    gb->registers[GB_REGISTER_AF] |= cycle_read_data_only(gb, gb->registers[register_id], "DRR", 1) << 8;
}

static void dec_rr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id = (opcode >> 4) + 1;
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "dec %r%", opcode, 1, opcode, register_names[register_id]);
    }

    cycle_oam_bug(gb, register_id);
    gb->registers[register_id]--;
}

static void inc_lr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    uint8_t value;
    register_id = (opcode >> 4) + 1;

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "inc %r%", opcode, 1, opcode, low_register_names[register_id]);
    }

    value = (gb->registers[register_id] & 0xFF) + 1;
    gb->registers[register_id] = (gb->registers[register_id] & 0xFF00) | value;

    gb->registers[GB_REGISTER_AF] &= ~(GB_SUBTRACT_FLAG | GB_ZERO_FLAG | GB_HALF_CARRY_FLAG);

    if ((gb->registers[register_id] & 0x0F) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((gb->registers[register_id] & 0xFF) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}
static void dec_lr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    uint8_t value;
    register_id = (opcode >> 4) + 1;

    value = (gb->registers[register_id] & 0xFF) - 1;

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "dec %r%", opcode, 1, opcode, low_register_names[register_id]);
    }

    gb->registers[register_id] = (gb->registers[register_id] & 0xFF00) | value;

    gb->registers[GB_REGISTER_AF] &= ~(GB_ZERO_FLAG | GB_HALF_CARRY_FLAG);
    gb->registers[GB_REGISTER_AF] |= GB_SUBTRACT_FLAG;

    if ((gb->registers[register_id] & 0x0F) == 0xF) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((gb->registers[register_id] & 0xFF) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void ld_lr_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    register_id = (opcode >> 4) + 1;

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        uint8_t value = cycle_read_inc_oam_bug(gb, gb->pc);
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int_registername(current_pc, "ld %r%, %int%", b2b1, 2, value, low_register_names[register_id]);
    }

    gb->registers[register_id] &= 0xFF00;
    gb->registers[register_id] |= cycle_read_inc_oam_bug(gb, gb->pc++);
}

static void rrca(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "rrca", opcode, 1);
    bool carry = (gb->registers[GB_REGISTER_AF] & 0x100) != 0;

    gb->registers[GB_REGISTER_AF] = (gb->registers[GB_REGISTER_AF] >> 1) & 0xFF00;
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG | 0x8000;
    }
}

static void rra(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "rra", opcode, 1);
    bool bit1 = (gb->registers[GB_REGISTER_AF] & 0x0100) != 0;
    bool carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;

    gb->registers[GB_REGISTER_AF] = (gb->registers[GB_REGISTER_AF] >> 1) & 0xFF00;
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= 0x8000;
    }
    if (bit1) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void jr_r8(GB_gameboy_t *gb, uint8_t opcode)
{
    /* Todo: Verify timing */
    int8_t operand = (int8_t)cycle_read_inc_oam_bug(gb, gb->pc);
    int16_t b2b1 = two_bytes_to_16bit_value(operand, opcode);

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        char buf[256];
        const char* label_name = libRR_log_jump_label(gb->pc+operand+1, current_pc);
        if (strcmp(label_name, "") == 0) {
            // printf("Invalid label name, could be in boot rom or hram\n");
        } else {
            snprintf(buf, sizeof(buf), "%s%s", "jr ", label_name);
            libRR_log_instruction(current_pc, buf, b2b1, 2);
        }
    }

    gb->pc += (int8_t)cycle_read_inc_oam_bug(gb, gb->pc) + 1;

    
    // gb->pc += operand + 1;
    cycle_no_access(gb);
}

static bool condition_code(GB_gameboy_t *gb, uint8_t opcode)
{
    switch ((opcode >> 3) & 0x3) {
        case 0:
            return !(gb->registers[GB_REGISTER_AF] & GB_ZERO_FLAG);
        case 1:
            return (gb->registers[GB_REGISTER_AF] & GB_ZERO_FLAG);
        case 2:
            return !(gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG);
        case 3:
            return (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG);
    }

    return false;
}

const char *condition_code_string(uint8_t opcode)
{
    switch ((opcode >> 3) & 0x3) {
        case 0:
            return "nz";
        case 1:
            return "z";
        case 2:
            return "nc";
        case 3:
            return "c";
    }

    return NULL;
}

static void jr_cc_r8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint32_t current_pc = gb->pc-1;
    int8_t offset = cycle_read_inc_oam_bug(gb, gb->pc++);

    int16_t b2b1 = two_bytes_to_16bit_value(offset, opcode);
    const char* cc = condition_code_string(opcode);

    if (condition_code(gb, opcode)) {
        // NOTE: we only log the jump if condition is true
        // this is because the label must be used otherwise assembler will crash due to referencing an undefined label
        // this means that if in your playthought you have never triggered this condition it will look like a nop with a comment say unexecuted
        char buf[256];
        
        const char* label_name = libRR_log_jump_label(gb->pc+offset, current_pc);
        if (strcmp(label_name, "") == 0) {
            // printf("Invalid label name, could be in boot rom or hram\n");
        } else {
            snprintf(buf, sizeof(buf), "jr %s, %s", cc, label_name);
            libRR_log_instruction(current_pc, buf, b2b1, 2);
        }
        gb->pc += offset;
        cycle_no_access(gb);
    } else {
        // TODO: how to log untaken condition codes
        char buf[256];
        snprintf(buf, sizeof(buf), "z_UNTAKEN_JUMP_2", cc);
        libRR_log_instruction(current_pc, buf, b2b1, 2);
    }
}

static void daa(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "daa", opcode, 1);
    int16_t result = gb->registers[GB_REGISTER_AF] >> 8;

    gb->registers[GB_REGISTER_AF] &= ~(0xFF00 | GB_ZERO_FLAG);

    if (gb->registers[GB_REGISTER_AF] & GB_SUBTRACT_FLAG) {
        if (gb->registers[GB_REGISTER_AF] & GB_HALF_CARRY_FLAG) {
            result = (result - 0x06) & 0xFF;
        }

        if (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) {
            result -= 0x60;
        }
    }
    else {
        if ((gb->registers[GB_REGISTER_AF] & GB_HALF_CARRY_FLAG) || (result & 0x0F) > 0x09) {
            result += 0x06;
        }

        if ((gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) || result > 0x9F) {
            result += 0x60;
        }
    }

    if ((result & 0xFF) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }

    if ((result & 0x100) == 0x100) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }

    gb->registers[GB_REGISTER_AF] &= ~GB_HALF_CARRY_FLAG;
    gb->registers[GB_REGISTER_AF] |= result << 8;
}

static void cpl(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "cpl", opcode, 1);
    gb->registers[GB_REGISTER_AF] ^= 0xFF00;
    gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG | GB_SUBTRACT_FLAG;
}

static void scf(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "scf", opcode, 1);
    gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    gb->registers[GB_REGISTER_AF] &= ~(GB_HALF_CARRY_FLAG | GB_SUBTRACT_FLAG);
}

static void ccf(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ccf", opcode, 1);
    gb->registers[GB_REGISTER_AF] ^= GB_CARRY_FLAG;
    gb->registers[GB_REGISTER_AF] &= ~(GB_HALF_CARRY_FLAG | GB_SUBTRACT_FLAG);
}

static void ld_dhli_a(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ld [hli], a", opcode, 1);
    cycle_write(gb, gb->registers[GB_REGISTER_HL]++, gb->registers[GB_REGISTER_AF] >> 8);
}

static void ld_dhld_a(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ld [hld], a", opcode, 1);
    cycle_write(gb, gb->registers[GB_REGISTER_HL]--, gb->registers[GB_REGISTER_AF] >> 8);
}

static void ld_a_dhli(GB_gameboy_t *gb, uint8_t opcode)
{
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "ld a, [hli]", opcode, 1, opcode, "a");
    }
    gb->registers[GB_REGISTER_AF] &= 0xFF;
    gb->registers[GB_REGISTER_AF] |= cycle_read_inc_oam_bug_data_only(gb, gb->registers[GB_REGISTER_HL]++, "[hl+]", 1) << 8;
}

static void ld_a_dhld(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ld a, [hld]", opcode, 1);
    gb->registers[GB_REGISTER_AF] &= 0xFF;
    gb->registers[GB_REGISTER_AF] |= cycle_read_inc_oam_bug_data_only(gb, gb->registers[GB_REGISTER_HL]--, "[hl-]", 1) << 8;
}

static void inc_dhl(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "inc [hl]", opcode, 1);
    uint8_t value;
    value = cycle_read_data_only(gb, gb->registers[GB_REGISTER_HL],"DHL",1) + 1;
    cycle_write(gb, gb->registers[GB_REGISTER_HL], value);

    gb->registers[GB_REGISTER_AF] &= ~(GB_SUBTRACT_FLAG | GB_ZERO_FLAG | GB_HALF_CARRY_FLAG);
    if ((value & 0x0F) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((value & 0xFF) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void dec_dhl(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "dec [hl]", opcode, 1);
    uint8_t value;
    value = cycle_read_data_only(gb, gb->registers[GB_REGISTER_HL], "DHL", 1) - 1;
    cycle_write(gb, gb->registers[GB_REGISTER_HL], value);

    gb->registers[GB_REGISTER_AF] &= ~( GB_ZERO_FLAG | GB_HALF_CARRY_FLAG);
    gb->registers[GB_REGISTER_AF] |= GB_SUBTRACT_FLAG;
    if ((value & 0x0F) == 0x0F) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((value & 0xFF) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void ld_dhl_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "ld_dhl_d8", opcode, 2);

    uint8_t data = cycle_read_inc_oam_bug(gb, gb->pc++);

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(data, opcode);
        libRR_log_instruction_1int(current_pc, "ld [hl], %int%", b2b1, 2, data);
    }

    cycle_write(gb, gb->registers[GB_REGISTER_HL], data);
}

static uint8_t get_src_value(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t src_register_id;
    uint8_t src_low;
    src_register_id = ((opcode >> 1) + 1) & 3;
    src_low = opcode & 1;
    if (src_register_id == GB_REGISTER_AF) {
        if (src_low) {
            return gb->registers[GB_REGISTER_AF] >> 8;
        }
        return cycle_read_data_only(gb, gb->registers[GB_REGISTER_HL], "HL src", 1);
    }
    if (src_low) {
        return gb->registers[src_register_id] & 0xFF;
    }
    return gb->registers[src_register_id] >> 8;
}

static void set_src_value(GB_gameboy_t *gb, uint8_t opcode, uint8_t value)
{
    uint8_t src_register_id;
    uint8_t src_low;
    src_register_id = ((opcode >> 1) + 1) & 3;
    src_low = opcode & 1;

    if (src_register_id == GB_REGISTER_AF) {
        if (src_low) {
            gb->registers[GB_REGISTER_AF] &= 0xFF;
            gb->registers[GB_REGISTER_AF] |= value << 8;
        }
        else {
            cycle_write(gb, gb->registers[GB_REGISTER_HL], value);
        }
    }
    else {
        if (src_low) {
            gb->registers[src_register_id] &= 0xFF00;
            gb->registers[src_register_id] |= value;
        }
        else {
            gb->registers[src_register_id] &= 0xFF;
            gb->registers[src_register_id] |= value << 8;
        }
    }
}

/* The LD r,r instruction is extremely common and extremely simple. Decoding this opcode at runtime is a significent
   performance hit, so we generate functions for every ld x,y couple (including [hl]) at compile time using macros. */

/* Todo: It's probably wise to do the same to all opcodes. */

#define LD_X_Y(x, y) \
static void ld_##x##_##y(GB_gameboy_t *gb, uint8_t opcode) \
{ \
libRR_log_instruction(gb->pc-1, "ld " #x ", " #y, opcode, 1); \
    gb->x = gb->y;\
}

#define LD_X_DHL(x) \
static void ld_##x##_##dhl(GB_gameboy_t *gb, uint8_t opcode) \
{ \
libRR_log_instruction(gb->pc-1, "ld " #x ", [hl]", opcode, 1); \
gb->x = cycle_read_data_only(gb, gb->registers[GB_REGISTER_HL], "DHL", 1); \
}

#define LD_DHL_Y(y) \
static void ld_##dhl##_##y(GB_gameboy_t *gb, uint8_t opcode) \
{ \
libRR_log_instruction(gb->pc-1, "ld [hl], " #y , opcode, 1); \
cycle_write(gb, gb->registers[GB_REGISTER_HL], gb->y); \
}

LD_X_Y(b,c) LD_X_Y(b,d) LD_X_Y(b,e) LD_X_Y(b,h) LD_X_Y(b,l)             LD_X_DHL(b) LD_X_Y(b,a)
LD_X_Y(c,b)             LD_X_Y(c,d) LD_X_Y(c,e) LD_X_Y(c,h) LD_X_Y(c,l) LD_X_DHL(c) LD_X_Y(c,a)
LD_X_Y(d,b) LD_X_Y(d,c)             LD_X_Y(d,e) LD_X_Y(d,h) LD_X_Y(d,l) LD_X_DHL(d) LD_X_Y(d,a)
LD_X_Y(e,b) LD_X_Y(e,c) LD_X_Y(e,d)             LD_X_Y(e,h) LD_X_Y(e,l) LD_X_DHL(e) LD_X_Y(e,a)
LD_X_Y(h,b) LD_X_Y(h,c) LD_X_Y(h,d) LD_X_Y(h,e)             LD_X_Y(h,l) LD_X_DHL(h) LD_X_Y(h,a)
LD_X_Y(l,b) LD_X_Y(l,c) LD_X_Y(l,d) LD_X_Y(l,e) LD_X_Y(l,h)             LD_X_DHL(l) LD_X_Y(l,a)
LD_DHL_Y(b) LD_DHL_Y(c) LD_DHL_Y(d) LD_DHL_Y(e) LD_DHL_Y(h) LD_DHL_Y(l)             LD_DHL_Y(a)
LD_X_Y(a,b) LD_X_Y(a,c) LD_X_Y(a,d) LD_X_Y(a,e) LD_X_Y(a,h) LD_X_Y(a,l) LD_X_DHL(a)

// fire the debugger if software breakpoints are enabled
static void ld_b_b(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ld_b_b", opcode, 2);
    if (gb->has_software_breakpoints) {
        gb->debug_stopped = true;
    }
}

static void add_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "add %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = (a + value) << 8;
    if ((uint8_t)(a + value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) + (value & 0xF) > 0x0F) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (((unsigned) a) + ((unsigned) value) > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void adc_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a, carry;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "adc %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;
    gb->registers[GB_REGISTER_AF] = (a + value + carry) << 8;

    if ((uint8_t)(a + value + carry) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) + (value & 0xF) + carry > 0x0F) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (((unsigned) a) + ((unsigned) value) + carry > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void sub_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "sub %r%", opcode, 1, opcode, get_src_name(opcode));
    }
    
    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = ((a - value) << 8) | GB_SUBTRACT_FLAG;
    if (a == value) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) < (value & 0xF)) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (a < value) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void sbc_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a, carry;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "sbc %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;
    gb->registers[GB_REGISTER_AF] = ((a - value - carry) << 8) | GB_SUBTRACT_FLAG;

    if ((uint8_t) (a - value - carry) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) < (value & 0xF) + carry) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (((unsigned) a) - ((unsigned) value) - carry > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void and_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "and %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = ((a & value) << 8) | GB_HALF_CARRY_FLAG;
    if ((a & value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void xor_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "xor %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = (a ^ value) << 8;
    if ((a ^ value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void or_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "or %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = (a | value) << 8;
    if ((a | value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void cp_a_r(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "CP a r", opcode, 1);
    uint8_t value, a;
    value = get_src_value(gb, opcode);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "cp %r%", opcode, 1, opcode, get_src_name(opcode));
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    gb->registers[GB_REGISTER_AF] |= GB_SUBTRACT_FLAG;
    if (a == value) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) < (value & 0xF)) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (a < value) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void halt(GB_gameboy_t *gb, uint8_t opcode)
{
    assert(gb->pending_cycles == 4);
    gb->pending_cycles = 0;
    GB_advance_cycles(gb, 4);

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "halt", opcode, 1, opcode, "");
    }
    
    gb->halted = true;
    /* Despite what some online documentations say, the HALT bug also happens on a CGB, in both CGB and DMG modes. */
    if (((gb->interrupt_enable & gb->io_registers[GB_IO_IF] & 0x1F) != 0)) {
        if (gb->ime) {
            gb->halted = false;
            gb->pc--;
        }
        else {
            gb->halted = false;
            gb->halt_bug = true;
        }
    }
    gb->just_halted = true;
}

static void pop_rr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;
    register_id = ((opcode >> 4) + 1) & 3;

    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "pop %r%", opcode, 1, opcode, register_names[register_id]);
    }

    gb->registers[register_id] = cycle_read_inc_oam_bug_data_only(gb, gb->registers[GB_REGISTER_SP]++, "SP", 2);
    gb->registers[register_id] |= cycle_read(gb, gb->registers[GB_REGISTER_SP]++) << 8;
    gb->registers[GB_REGISTER_AF] &= 0xFFF0; // Make sure we don't set impossible flags on F! See Blargg's PUSH AF test.
}

static void jp_cc_a16(GB_gameboy_t *gb, uint8_t opcode)
{
    uint32_t current_pc = gb->pc-1;
    uint8_t operand1 = cycle_read_inc_oam_bug(gb, gb->pc);
    uint8_t operand2 = cycle_read_inc_oam_bug(gb, gb->pc+1);
    uint16_t addr = cycle_read_inc_oam_bug(gb, gb->pc++);
    addr |= (cycle_read_inc_oam_bug(gb, gb->pc++) << 8);
    if (condition_code(gb, opcode)) {

        // log three byte instruction
        if (libRR_full_function_log) {
            
            uint32_t b2b1 = three_bytes_to_24bit_value(operand2, (uint8_t)operand1, opcode);
            int16_t both_operands = operand1;
            both_operands |= operand2 << 8;
            libRR_log_instruction_1int_registername(current_pc, "jp %r%, %int%", b2b1, 3, both_operands, get_condition_code_name(opcode));
        }

        // Only log jump if condition is true
        libRR_log_long_jump(current_pc, addr, "jp_cc_a16");
        cycle_no_access(gb);
        gb->pc = addr;
    } else {
        libRR_log_instruction(current_pc, "z_UNTAKEN_LONG_JUMP", opcode, 3);
    }
}

static void jp_a16(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "JP a16", opcode, 3);
    uint16_t addr = cycle_read_inc_oam_bug(gb, gb->pc);
    addr |= (cycle_read_inc_oam_bug(gb, gb->pc + 1) << 8);

    // log three byte instruction
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        uint8_t operand1 = cycle_read_inc_oam_bug(gb, gb->pc);
        uint8_t operand2 = cycle_read_inc_oam_bug(gb, gb->pc+1);
        uint32_t b2b1 = three_bytes_to_24bit_value(operand2, (uint8_t)operand1, opcode);
        int16_t both_operands = operand1;
        both_operands |= operand2 << 8;
        libRR_log_instruction_1int_registername(current_pc, "jp %int%", b2b1, 3, both_operands, "");
    }

    libRR_log_long_jump(gb->pc-1, addr, "jp_a16");
    cycle_no_access(gb);
    gb->pc = addr;
    
}

static void call_cc_a16(GB_gameboy_t *gb, uint8_t opcode)
{
    uint32_t current_pc = gb->pc-1;
    uint8_t operand1 = cycle_read_inc_oam_bug(gb, gb->pc);
    uint8_t operand2 = cycle_read_inc_oam_bug(gb, gb->pc+1);

    uint16_t call_addr = gb->pc - 1;
    uint16_t addr = cycle_read_inc_oam_bug(gb, gb->pc++);
    addr |= (cycle_read_inc_oam_bug(gb, gb->pc++) << 8);
    
    if (condition_code(gb, opcode)) {
        // log three byte instruction
        if (libRR_full_function_log) {
            uint32_t b2b1 = three_bytes_to_24bit_value(operand2, (uint8_t)operand1, opcode);
            int16_t both_operands = operand1;
            both_operands |= operand2 << 8;
            libRR_log_instruction_1int_registername(current_pc, "call %r%, %int%", b2b1, 3, both_operands, get_condition_code_name(opcode));
        }
        // libRR start
        libRR_log_function_call(call_addr, addr, gb->registers[GB_REGISTER_SP]);
        // libRR end
        cycle_oam_bug(gb, GB_REGISTER_SP);
        cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) >> 8);
        cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) & 0xFF);
        gb->pc = addr;

        GB_debugger_call_hook(gb, call_addr);
    } else {
        libRR_log_instruction(current_pc, "call z_UNCALLED_FUNCTION", opcode, 3);
    }
}

static void push_rr(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t register_id;

    cycle_oam_bug(gb, GB_REGISTER_SP);
    register_id = ((opcode >> 4) + 1) & 3;
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "push %r%", opcode, 1, opcode, register_names[register_id]);
    }
    cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->registers[register_id]) >> 8);
    cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->registers[register_id]) & 0xFF);
}

static void add_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "add %int%", b2b1, 2, value);
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = (a + value) << 8;
    if ((uint8_t) (a + value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) + (value & 0xF) > 0x0F) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (((unsigned) a) + ((unsigned) value) > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void adc_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a, carry;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);
    a = gb->registers[GB_REGISTER_AF] >> 8;
    carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;
    gb->registers[GB_REGISTER_AF] = (a + value + carry) << 8;

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "adc a, %int%", b2b1, 2, value);
    }

    if (gb->registers[GB_REGISTER_AF] == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) + (value & 0xF) + carry > 0x0F) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (((unsigned) a) + ((unsigned) value) + carry > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void sub_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "sub a,%int%", b2b1, 2, value);
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = ((a - value) << 8) | GB_SUBTRACT_FLAG;
    if (a == value) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) < (value & 0xF)) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (a < value) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void sbc_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a, carry;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);
    a = gb->registers[GB_REGISTER_AF] >> 8;
    carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;
    gb->registers[GB_REGISTER_AF] = ((a - value - carry) << 8) | GB_SUBTRACT_FLAG;

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "sbc %int%", b2b1, 2, value);
    }

    if ((uint8_t) (a - value - carry) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) < (value & 0xF) + carry) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (((unsigned) a) - ((unsigned) value) - carry > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void and_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "and %int%", b2b1, 2, value);
    }
    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = ((a & value) << 8) | GB_HALF_CARRY_FLAG;
    if ((a & value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void xor_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "xor a, %int%", b2b1, 2, value);
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = (a ^ value) << 8;
    if ((a ^ value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void or_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value, a;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "or %int%", b2b1, 2, value);
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] = (a | value) << 8;
    if ((a | value) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void cp_a_d8(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "CP a d8", opcode, 2);
    uint8_t value, a;
    value = cycle_read_inc_oam_bug(gb, gb->pc++);
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(value, opcode);
        libRR_log_instruction_1int(current_pc, "cp %int%", b2b1, 2, value);
    }

    a = gb->registers[GB_REGISTER_AF] >> 8;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    gb->registers[GB_REGISTER_AF] |= GB_SUBTRACT_FLAG;
    if (a == value) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
    if ((a & 0xF) < (value & 0xF)) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }
    if (a < value) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void rst(GB_gameboy_t *gb, uint8_t opcode)
{
    uint16_t call_addr = gb->pc - 1;
    libRR_log_instruction(gb->pc-1, "RST", opcode, 1);
    cycle_oam_bug(gb, GB_REGISTER_SP);
    cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) >> 8);
    cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) & 0xFF);
    gb->pc = opcode ^ 0xC7;
    // libRR start (not sure if RST should be counted as a call)
    // printf("RST");
    // libRR_log_function_call(opcode ^ 0xC7, gb->pc - 1, gb->registers[GB_REGISTER_SP]);
    // libRR end
    GB_debugger_call_hook(gb, call_addr);
}

static void _ret(GB_gameboy_t *gb, uint8_t opcode, bool was_conditional)
{
    GB_debugger_ret_hook(gb);
    uint16_t call_addr = gb->pc - 1;
    if (!was_conditional) {
        libRR_log_instruction(gb->pc - 1, "RET", opcode, 1);
    }
    gb->pc = cycle_read_inc_oam_bug_data_only(gb, gb->registers[GB_REGISTER_SP]++, "RET SP+", 2);
    gb->pc |= cycle_read(gb, gb->registers[GB_REGISTER_SP]++) << 8;
    // libRR start
    // Why do we subtract 3?
    libRR_log_return_statement(call_addr, gb->pc-3, gb->registers[GB_REGISTER_SP]);
    // libRR end
    cycle_no_access(gb);
}

static void ret(GB_gameboy_t *gb, uint8_t opcode) {
    _ret(gb, opcode, false);
}


static void reti(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "reti", opcode, 1);
    _ret(gb, opcode, true);
    gb->ime = true;
}

static void ret_cc(GB_gameboy_t *gb, uint8_t opcode)
{
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "ret %r%", opcode, 1, opcode, get_condition_code_name(opcode));
    }
    if (condition_code(gb, opcode)) {
        cycle_no_access(gb);
        _ret(gb, opcode, true);
    }
    else {
        // TODO: still log as an unused ret instruction
        cycle_no_access(gb);
    }
}

static void call_a16(GB_gameboy_t *gb, uint8_t opcode)
{
    //libRR_log_instruction(gb->pc-1, "CALL a16", opcode, 3);

    // log three byte instruction
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        uint8_t operand1 = cycle_read_inc_oam_bug(gb, gb->pc);
        uint8_t operand2 = cycle_read_inc_oam_bug(gb, gb->pc+1);
        uint32_t b2b1 = three_bytes_to_24bit_value(operand2, (uint8_t)operand1, opcode);
        int16_t both_operands = operand1;
        both_operands |= operand2 << 8;
        libRR_log_instruction_1int_registername(current_pc, "call %int%", b2b1, 3, both_operands, "");
    }

    uint16_t call_addr = gb->pc - 1;
    uint16_t addr = cycle_read_inc_oam_bug(gb, gb->pc++);
    addr |= (cycle_read_inc_oam_bug(gb, gb->pc++) << 8);
    // libRR start
    libRR_log_function_call(call_addr, addr, gb->registers[GB_REGISTER_SP]);
    // libRR end
    cycle_oam_bug(gb, GB_REGISTER_SP);
    cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) >> 8);
    cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) & 0xFF);
    gb->pc = addr;
    GB_debugger_call_hook(gb, call_addr);
}

// This is an LDH (2 bytes)  (E0)
static void ld_da8_a(GB_gameboy_t *gb, uint8_t opcode)
{
    uint32_t current_pc = gb->pc-1;
    uint8_t operand = cycle_read_inc_oam_bug(gb, gb->pc++);
    int16_t b2b1 = two_bytes_to_16bit_value(operand, opcode);
    libRR_log_instruction_1int(current_pc, "ldh [%int%], a", b2b1, 2, operand);

    cycle_write(gb, 0xFF00 + operand, gb->registers[GB_REGISTER_AF] >> 8);
}

// This is an LDH (2 bytes) (f0)
static void ld_a_da8(GB_gameboy_t *gb, uint8_t opcode)
{
    uint32_t current_pc = gb->pc-1;
    gb->registers[GB_REGISTER_AF] &= 0xFF;
    uint8_t operand = cycle_read_inc_oam_bug(gb, gb->pc++);
    int16_t b2b1 = two_bytes_to_16bit_value(operand, opcode);
    libRR_log_instruction_1int(current_pc, "ldh a, [%int%]", b2b1, 2, operand);
    gb->registers[GB_REGISTER_AF] |= cycle_read_data_only(gb, 0xFF00 + operand, "da8", 1) << 8;
}

static void ld_dc_a(GB_gameboy_t *gb, uint8_t opcode)
{
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "ld [c], a", opcode, 1, opcode, "a");
    }

    cycle_write(gb, 0xFF00 + (gb->registers[GB_REGISTER_BC] & 0xFF), gb->registers[GB_REGISTER_AF] >> 8);
}

static void ld_a_dc(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ld a, [c]", opcode, 1);
    gb->registers[GB_REGISTER_AF] &= 0xFF;
    gb->registers[GB_REGISTER_AF] |= cycle_read_data_only(gb, 0xFF00 + (gb->registers[GB_REGISTER_BC] & 0xFF), "DC", 1) << 8;
}

static void add_sp_r8(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "ADD sp, r8", opcode, 2);
    int16_t offset;
    uint16_t sp = gb->registers[GB_REGISTER_SP];
    offset = (int8_t) cycle_read_inc_oam_bug(gb, gb->pc++);
    cycle_no_access(gb);
    cycle_no_access(gb);
    gb->registers[GB_REGISTER_SP] += offset;

    gb->registers[GB_REGISTER_AF] &= 0xFF00;

    /* A new instruction, a new meaning for Half Carry! */
    if ((sp & 0xF) + (offset & 0xF) > 0xF) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((sp & 0xFF) + (offset & 0xFF) > 0xFF)  {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void jp_hl(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "jp hl", opcode, 1);

    libRR_log_long_jump(gb->pc-1, gb->registers[GB_REGISTER_HL], "jp_hl");
    gb->pc = gb->registers[GB_REGISTER_HL];
}

static void ld_da16_a(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "ld_da16_a", opcode, 3);

    // log three byte instruction
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        uint8_t operand1 = cycle_read_inc_oam_bug(gb, gb->pc);
        uint8_t operand2 = cycle_read_inc_oam_bug(gb, gb->pc+1);
        uint32_t b2b1 = three_bytes_to_24bit_value(operand2, (uint8_t)operand1, opcode);
        int16_t both_operands = operand1;
        both_operands |= operand2 << 8;
        libRR_log_instruction_1int_registername(current_pc, "ld [%int%], a", b2b1, 3, both_operands, "a");
    }

    uint16_t addr;
    addr = cycle_read_inc_oam_bug(gb, gb->pc++);
    addr |= cycle_read_inc_oam_bug(gb, gb->pc++) << 8;
    cycle_write(gb, addr, gb->registers[GB_REGISTER_AF] >> 8);
}

static void ld_a_da16(GB_gameboy_t *gb, uint8_t opcode)
{
    uint16_t addr;
    gb->registers[GB_REGISTER_AF] &= 0xFF;

    // log three byte instruction
    if (libRR_full_function_log) {
        uint8_t operand1 = cycle_read_inc_oam_bug(gb, gb->pc);
        uint8_t operand2 = cycle_read_inc_oam_bug(gb, gb->pc+1);
        uint32_t current_pc = gb->pc-1;
        uint32_t b2b1 = three_bytes_to_24bit_value(operand2, (uint8_t)operand1, opcode);
        int16_t both_operands = operand1;
        both_operands |= operand2 << 8;
        libRR_log_instruction_1int_registername(current_pc, "ld a, [%int%]", b2b1, 3, both_operands, "a");
    }

    addr = cycle_read_inc_oam_bug(gb, gb->pc++);
    addr |= cycle_read_inc_oam_bug(gb, gb->pc++) << 8;

    gb->registers[GB_REGISTER_AF] |= cycle_read_data_only(gb, addr, "da16", 1) << 8;
}

static void di(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "DI", opcode, 1);
    /* DI is NOT delayed, not even on a CGB. Mooneye's di_timing-GS test fails on a CGB
       for different reasons. */
    gb->ime = false;
}

static void ei(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "EI", opcode, 1);
    /* ei is actually "disable interrupts for one instruction, then enable them". */
    if (!gb->ime && !gb->ime_toggle) {
        gb->ime_toggle = true;
    }
}

static void ld_hl_sp_r8(GB_gameboy_t *gb, uint8_t opcode)
{
    int16_t offset;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    offset = (int8_t) cycle_read_inc_oam_bug(gb, gb->pc++);
    cycle_no_access(gb);
    gb->registers[GB_REGISTER_HL] = gb->registers[GB_REGISTER_SP] + offset;

    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-2;
        int16_t b2b1 = two_bytes_to_16bit_value(offset, opcode);
        libRR_log_instruction_1int(current_pc, "ld hl, sp + %int%", b2b1, 2, offset);
    }

    if ((gb->registers[GB_REGISTER_SP] & 0xF) + (offset & 0xF) > 0xF) {
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
    }

    if ((gb->registers[GB_REGISTER_SP] & 0xFF)  + (offset & 0xFF) > 0xFF) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
}

static void ld_sp_hl(GB_gameboy_t *gb, uint8_t opcode)
{
    // 1 byte register instruction logging
    if (libRR_full_function_log) {
        uint32_t current_pc = gb->pc-1;
        libRR_log_instruction_1int_registername(current_pc, "ld sp, hl", opcode, 1, opcode, "hl");
    }
    gb->registers[GB_REGISTER_SP] = gb->registers[GB_REGISTER_HL];
    cycle_no_access(gb);
}

static void rlc_r(GB_gameboy_t *gb, uint8_t opcode)
{
    libRR_log_instruction(gb->pc-1, "RLC r", opcode, 1);
    bool carry;
    uint8_t value;
    value = get_src_value(gb, opcode);
    carry = (value & 0x80) != 0;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    set_src_value(gb, opcode, (value << 1) | carry);
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    if (!(value << 1)) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void rrc_r(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_ logged elsewhere
    bool carry;
    uint8_t value;
    value = get_src_value(gb, opcode);
    carry = (value & 0x01) != 0;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    value = (value >> 1) | (carry << 7);
    set_src_value(gb, opcode, value);
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    if (value == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void rl_r(GB_gameboy_t *gb, uint8_t opcode)
{
    bool carry;
    uint8_t value;
    bool bit7;
    value = get_src_value(gb, opcode);
    carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;
    bit7 = (value & 0x80) != 0;

    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    value = (value << 1) | carry;
    set_src_value(gb, opcode, value);
    if (bit7) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    if (value == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void rr_r(GB_gameboy_t *gb, uint8_t opcode)
{
    bool carry;
    uint8_t value;
    bool bit1;

    value = get_src_value(gb, opcode);
    carry = (gb->registers[GB_REGISTER_AF] & GB_CARRY_FLAG) != 0;
    bit1 = (value & 0x1) != 0;

    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    value = (value >> 1) | (carry << 7);
    set_src_value(gb, opcode, value);
    if (bit1) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    if (value == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void sla_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value;
    bool carry;
    value = get_src_value(gb, opcode);
    carry = (value & 0x80) != 0;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    set_src_value(gb, opcode, (value << 1));
    if (carry) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    if ((value & 0x7F) == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void sra_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t bit7;
    uint8_t value;
    value = get_src_value(gb, opcode);
    bit7 = value & 0x80;
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    if (value & 1) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    value = (value >> 1) | bit7;
    set_src_value(gb, opcode, value);
    if (value == 0) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void srl_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value;
    value = get_src_value(gb, opcode);
    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    set_src_value(gb, opcode, (value >> 1));
    if (value & 1) {
        gb->registers[GB_REGISTER_AF] |= GB_CARRY_FLAG;
    }
    if (!(value >> 1)) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void swap_r(GB_gameboy_t *gb, uint8_t opcode)
{
    uint8_t value;
    value = get_src_value(gb, opcode);

    gb->registers[GB_REGISTER_AF] &= 0xFF00;
    set_src_value(gb, opcode, (value >> 4) | (value << 4));
    if (!value) {
        gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
    }
}

static void bit_r(GB_gameboy_t *gb, uint8_t opcode)
{
    // libRR_log_instruction(gb->pc-1, "BIT r", opcode, 1);
    uint8_t value;
    uint8_t bit;
    value = get_src_value(gb, opcode);
    bit = 1 << ((opcode >> 3) & 7);
    if ((opcode & 0xC0) == 0x40) { /* Bit */
        gb->registers[GB_REGISTER_AF] &= 0xFF00 | GB_CARRY_FLAG;
        gb->registers[GB_REGISTER_AF] |= GB_HALF_CARRY_FLAG;
        if (!(bit & value)) {
            gb->registers[GB_REGISTER_AF] |= GB_ZERO_FLAG;
        }
    }
    else if ((opcode & 0xC0) == 0x80) { /* res */
        set_src_value(gb, opcode, value & ~bit);
    }
    else if ((opcode & 0xC0) == 0xC0) { /* set */
        set_src_value(gb, opcode, value | bit);
    }
}


// special CB functions

const char *get_src_name(uint8_t opcode)
{
    uint8_t src_register_id;
    uint8_t src_low;
    src_register_id = ((opcode >> 1) + 1) & 3;
    src_low = (opcode & 1);
    if (src_register_id == GB_REGISTER_AF) {
        return src_low? "a": "[hl]";
    }
    if (src_low) {
        return register_names[src_register_id] + 1;
    }
    static const char *high_register_names[] = {"a", "b", "d", "h"};
    return high_register_names[src_register_id];
}

void libRR_log_bit_r(GB_gameboy_t *gb, uint8_t original_opcode, uint8_t opcode, uint16_t pc)
{
    if (!libRR_full_function_log || !libRR_finished_boot_rom) {
        return;
    }

    char bit_as_string[2];
    uint8_t bit = ((opcode >> 3) & 7);
    sprintf(bit_as_string, "%d", bit);
    int16_t b2b1 = two_bytes_to_16bit_value(opcode, original_opcode);
    if ((opcode & 0xC0) == 0x40) { /* Bit */
        libRR_log_instruction_z80_s_d(pc, "bit %d%, %s%", b2b1, 2, get_src_name(opcode), bit_as_string);
        //GB_log(gb, "BIT %s, %d\n",  get_src_name(opcode), bit);
    }
    else if ((opcode & 0xC0) == 0x80) { /* res */
        // GB_log(gb, "RES %s, %d\n",  get_src_name(opcode), bit);
        libRR_log_instruction_z80_s_d(pc, "res %d%, %s%", b2b1, 2, get_src_name(opcode), bit_as_string);
    }
    else if ((opcode & 0xC0) == 0xC0) { /* set */
        // GB_log(gb, "SET %s, %d\n",  get_src_name(opcode), bit);
        libRR_log_instruction_z80_s_d(pc, "set %d%, %s%", b2b1, 2, get_src_name(opcode), bit_as_string);
    }
}

static void cb_prefix(GB_gameboy_t *gb, uint8_t original_opcode)
{
    int16_t current_pc = gb->pc-1;

    uint8_t opcode = cycle_read_inc_oam_bug(gb, gb->pc++);
    int16_t b2b1 = two_bytes_to_16bit_value(opcode, original_opcode);
    switch (opcode >> 3) {
        case 0:
        // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "rlc %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            rlc_r(gb, opcode);
            break;
        case 1:
            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "rrc %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            rrc_r(gb, opcode);
            break;
        case 2:
            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "rl %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            rl_r(gb, opcode);
            break;
        case 3:
            // libRR_log_instruction(current_pc, "cb_rr_r", b2b1, 2);

            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "rr %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }

            rr_r(gb, opcode);
            break;
        case 4:
            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "sla %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            sla_r(gb, opcode);
            break;
        case 5:
            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "sra %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            sra_r(gb, opcode);
            break;
        case 6:
            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2; // not sure why swap needs to be -2
                libRR_log_instruction_1int_registername(current_pc, "swap %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            swap_r(gb, opcode);
            break;
        case 7:
            //libRR_log_instruction(current_pc, "cb_srl_r", b2b1, 2);
            // CB 2 byte register instruction logging
            if (libRR_full_function_log) {
                uint32_t current_pc = gb->pc-2;
                libRR_log_instruction_1int_registername(current_pc, "srl %r%", b2b1, 2, original_opcode, get_src_name(opcode));
            }
            srl_r(gb, opcode);
            break;
        default:
            ///libRR_log_instruction(current_pc, "cb_bit_r", b2b1, 2);
            libRR_log_bit_r(gb, original_opcode, opcode, current_pc);
            bit_r(gb, opcode);
            break;
    }
}



static GB_opcode_t *opcodes[256] = {
    /*  X0          X1          X2          X3          X4          X5          X6          X7                */
    /*  X8          X9          Xa          Xb          Xc          Xd          Xe          Xf                */
    nop,        ld_rr_d16,  ld_drr_a,   inc_rr,     inc_hr,     dec_hr,     ld_hr_d8,   rlca,       /* 0X */
    ld_da16_sp, add_hl_rr,  ld_a_drr,   dec_rr,     inc_lr,     dec_lr,     ld_lr_d8,   rrca,
    stop,       ld_rr_d16,  ld_drr_a,   inc_rr,     inc_hr,     dec_hr,     ld_hr_d8,   rla,        /* 1X */
    jr_r8,      add_hl_rr,  ld_a_drr,   dec_rr,     inc_lr,     dec_lr,     ld_lr_d8,   rra,
    jr_cc_r8,   ld_rr_d16,  ld_dhli_a,  inc_rr,     inc_hr,     dec_hr,     ld_hr_d8,   daa,        /* 2X */
    jr_cc_r8,   add_hl_rr,  ld_a_dhli,  dec_rr,     inc_lr,     dec_lr,     ld_lr_d8,   cpl,
    jr_cc_r8,   ld_rr_d16,  ld_dhld_a,  inc_rr,     inc_dhl,    dec_dhl,    ld_dhl_d8,  scf,        /* 3X */
    jr_cc_r8,   add_hl_rr,  ld_a_dhld,  dec_rr,     inc_hr,     dec_hr,     ld_hr_d8,   ccf,
    ld_b_b,     ld_b_c,     ld_b_d,     ld_b_e,     ld_b_h,     ld_b_l,     ld_b_dhl,   ld_b_a,     /* 4X */
    ld_c_b,     nop,        ld_c_d,     ld_c_e,     ld_c_h,     ld_c_l,     ld_c_dhl,   ld_c_a,
    ld_d_b,     ld_d_c,     nop,        ld_d_e,     ld_d_h,     ld_d_l,     ld_d_dhl,   ld_d_a,     /* 5X */
    ld_e_b,     ld_e_c,     ld_e_d,     nop,        ld_e_h,     ld_e_l,     ld_e_dhl,   ld_e_a,
    ld_h_b,     ld_h_c,     ld_h_d,     ld_h_e,     nop,        ld_h_l,     ld_h_dhl,   ld_h_a,     /* 6X */
    ld_l_b,     ld_l_c,     ld_l_d,     ld_l_e,     ld_l_h,     nop,        ld_l_dhl,   ld_l_a,
    ld_dhl_b,   ld_dhl_c,   ld_dhl_d,   ld_dhl_e,   ld_dhl_h,   ld_dhl_l,   halt,       ld_dhl_a,   /* 7X */
    ld_a_b,     ld_a_c,     ld_a_d,     ld_a_e,     ld_a_h,     ld_a_l,     ld_a_dhl,   nop,
    add_a_r,    add_a_r,    add_a_r,    add_a_r,    add_a_r,    add_a_r,    add_a_r,    add_a_r,    /* 8X */
    adc_a_r,    adc_a_r,    adc_a_r,    adc_a_r,    adc_a_r,    adc_a_r,    adc_a_r,    adc_a_r,
    sub_a_r,    sub_a_r,    sub_a_r,    sub_a_r,    sub_a_r,    sub_a_r,    sub_a_r,    sub_a_r,    /* 9X */
    sbc_a_r,    sbc_a_r,    sbc_a_r,    sbc_a_r,    sbc_a_r,    sbc_a_r,    sbc_a_r,    sbc_a_r,
    and_a_r,    and_a_r,    and_a_r,    and_a_r,    and_a_r,    and_a_r,    and_a_r,    and_a_r,    /* aX */
    xor_a_r,    xor_a_r,    xor_a_r,    xor_a_r,    xor_a_r,    xor_a_r,    xor_a_r,    xor_a_r,
    or_a_r,     or_a_r,     or_a_r,     or_a_r,     or_a_r,     or_a_r,     or_a_r,     or_a_r,     /* bX */
    cp_a_r,     cp_a_r,     cp_a_r,     cp_a_r,     cp_a_r,     cp_a_r,     cp_a_r,     cp_a_r,
    ret_cc,     pop_rr,     jp_cc_a16,  jp_a16,     call_cc_a16,push_rr,    add_a_d8,   rst,        /* cX */
    ret_cc,     ret,        jp_cc_a16,  cb_prefix,  call_cc_a16,call_a16,   adc_a_d8,   rst,
    ret_cc,     pop_rr,     jp_cc_a16,  ill,        call_cc_a16,push_rr,    sub_a_d8,   rst,        /* dX */
    ret_cc,     reti,       jp_cc_a16,  ill,        call_cc_a16,ill,        sbc_a_d8,   rst,
    ld_da8_a,   pop_rr,     ld_dc_a,    ill,        ill,        push_rr,    and_a_d8,   rst,        /* eX */
    add_sp_r8,  jp_hl,      ld_da16_a,  ill,        ill,        ill,        xor_a_d8,   rst,
    ld_a_da8,   pop_rr,     ld_a_dc,    di,         ill,        push_rr,    or_a_d8,    rst,        /* fX */
    ld_hl_sp_r8,ld_sp_hl,   ld_a_da16,  ei,         ill,        ill,        cp_a_d8,    rst,
};
void GB_cpu_run(GB_gameboy_t *gb)
{
    if (gb->hdma_on) {
        GB_advance_cycles(gb, 4);
        return;
    }
    if (gb->stopped) {
        GB_timing_sync(gb);
        GB_advance_cycles(gb, 4);
        if ((gb->io_registers[GB_IO_JOYP] & 0xF) != 0xF) {
            leave_stop_mode(gb);
            GB_advance_cycles(gb, 8);
        }
        return;
    }
    
    if ((gb->interrupt_enable & 0x10) && (gb->ime || gb->halted)) {
        GB_timing_sync(gb);
    }
    
    if (gb->halted && !GB_is_cgb(gb) && !gb->just_halted) {
        GB_advance_cycles(gb, 2);
    }
    
    uint8_t interrupt_queue = gb->interrupt_enable & gb->io_registers[GB_IO_IF] & 0x1F;
    
    if (gb->halted) {
        GB_advance_cycles(gb, (GB_is_cgb(gb) || gb->just_halted) ? 4 : 2);
    }
    gb->just_halted = false;

    bool effective_ime = gb->ime;
    if (gb->ime_toggle) {
        gb->ime = !gb->ime;
        gb->ime_toggle = false;
    }

    /* Wake up from HALT mode without calling interrupt code. */
    if (gb->halted && !effective_ime && interrupt_queue) {
        gb->halted = false;
    }
    
    /* Call interrupt */
    else if (effective_ime && interrupt_queue) {
        gb->halted = false;
        uint16_t call_addr = gb->pc;
        cycle_no_access(gb);
        cycle_no_access(gb);
        GB_trigger_oam_bug(gb, gb->registers[GB_REGISTER_SP]); /* Todo: test T-cycle timing */
        cycle_no_access(gb);
        
        cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) >> 8);
        interrupt_queue = gb->interrupt_enable;
        
        if (gb->registers[GB_REGISTER_SP] == GB_IO_IF + 0xFF00 + 1) {
            gb->registers[GB_REGISTER_SP]--;
            interrupt_queue &= cycle_write_if(gb, (gb->pc) & 0xFF);
        }
        else {
            cycle_write(gb, --gb->registers[GB_REGISTER_SP], (gb->pc) & 0xFF);
            interrupt_queue &= (gb->io_registers[GB_IO_IF]) & 0x1F;
        }
        
        if (interrupt_queue) {
            uint8_t interrupt_bit = 0;
            while (!(interrupt_queue & 1)) {
                interrupt_queue >>= 1;
                interrupt_bit++;
            }
            gb->io_registers[GB_IO_IF] &= ~(1 << interrupt_bit);
            gb->pc = interrupt_bit * 8 + 0x40;
        }
        else {
            gb->pc = 0;
        }
        gb->ime = false;
        GB_debugger_call_hook(gb, call_addr);
         // libRR Start call for Interrupt
        // printf("Interuppt call: %s\n", n2hexstr(call_addr));
        libRR_log_interrupt_call(call_addr, gb->pc);
        // libRR_log_function_call(call_addr, gb->pc, gb->registers[GB_REGISTER_SP]);
        // libRR end
        // printf("libRR TODO: what is call interrupt? call_addr:%d\n", call_addr);
    }
    /* Run mode */
    else if (!gb->halted) {
        gb->last_opcode_read = cycle_read_inc_oam_bug(gb, gb->pc++);
        if (gb->halt_bug) {
            gb->pc--;
            gb->halt_bug = false;
        }
        opcodes[gb->last_opcode_read](gb, gb->last_opcode_read);
    }
    
    flush_pending_cycles(gb);

    if (gb->hdma_starting) {
        gb->hdma_starting = false;
        gb->hdma_on = true;
        gb->hdma_cycles = -8;
    }
}
