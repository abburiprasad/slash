#include "compile.h"
#include "string.h"

typedef struct fixup {
    struct fixup* next;
    size_t fixup;
}
fixup_t;

typedef struct next_last_frame {
    struct next_last_frame* prev;
    fixup_t* next_fixups;
    fixup_t* last_fixups;
}
next_last_frame_t;

typedef struct sl_compile_state {
    sl_vm_t* vm;
    st_table_t* vars;
    struct sl_compile_state* parent;
    uint8_t* registers;
    sl_vm_section_t* section;
    next_last_frame_t* next_last_frames;
}
sl_compile_state_t;

static size_t
reg_alloc(sl_compile_state_t* cs)
{
    size_t i;
    for(i = 0; i < cs->section->max_registers; i++) {
        if(!cs->registers[i]) {
            cs->registers[i] = 1;
            return i;
        }
    }
    cs->section->max_registers++;
    cs->registers = sl_realloc(cs->vm->arena, cs->registers, cs->section->max_registers);
    return cs->section->max_registers - 1;
}

static size_t
reg_alloc_block(sl_compile_state_t* cs, size_t count)
{
    size_t i, j;
    size_t begin;
    for(i = 0; i < cs->section->max_registers - count + 1; i++) {
        if(cs->registers[i]) {
            continue_outer: continue;
        }
        begin = i;
        for(j = 0; j < count; j++) {
            if(cs->registers[i + j]) {
                i += j;
                goto continue_outer;
            }
        }
        for(j = 0; j < count; j++) {
            cs->registers[i + j] = 1;
        }
        return i;
    }
    cs->section->max_registers += count;
    cs->registers = sl_realloc(cs->vm->arena, cs->registers, cs->section->max_registers);
    return cs->section->max_registers - count;
}

static void
reg_free(sl_compile_state_t* cs, size_t reg)
{
    cs->registers[reg] = 0;
}

static void
reg_free_block(sl_compile_state_t* cs, size_t reg, size_t count)
{
    size_t i;
    for(i = 0; i < count; i++) {
        reg_free(cs, reg + i);
    }
}

static size_t
emit(sl_compile_state_t* cs, sl_vm_insn_t insn)
{
    if(cs->section->insns_count == cs->section->insns_cap) {
        cs->section->insns_cap *= 2;
        cs->section->insns = sl_realloc(cs->vm->arena, cs->section->insns, sizeof(sl_vm_insn_t) * cs->section->insns_cap);
    }
    cs->section->insns[cs->section->insns_count++] = insn;
    return cs->section->insns_count - 1;
}

static void
emit_immediate(sl_compile_state_t* cs, SLVAL immediate, size_t dest)
{
    sl_vm_insn_t insn;
    insn.opcode = SL_OP_IMMEDIATE;
    emit(cs, insn);
    insn.imm = immediate;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

static void
compile_node(sl_compile_state_t* cs, sl_node_base_t* node, size_t dest);

#define NODE(type, name) static void compile_##name(sl_compile_state_t* cs, type* node, size_t dest)

NODE(sl_node_seq_t, seq)
{
    size_t i;
    for(i = 0; i < node->node_count; i++) {
        /*  we only care about the result of the last node so we'll write all
            results to the same output register we were given */
        compile_node(cs, node->nodes[i], dest);
    }
    if(node->node_count == 0) {
        emit_immediate(cs, cs->vm->lib.nil, dest);
    }
}

NODE(sl_node_raw_t, raw)
{
    sl_vm_insn_t insn;
    insn.opcode = SL_OP_RAW;
    emit(cs, insn);
    insn.imm = node->string;
    emit(cs, insn);
    (void)dest;
}

NODE(sl_node_echo_t, echo)
{
    sl_vm_insn_t insn;
    compile_node(cs, node->expr, dest);
    insn.opcode = SL_OP_ECHO;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_echo_t, echo_raw)
{
    sl_vm_insn_t insn;
    compile_node(cs, node->expr, dest);
    insn.opcode = SL_OP_ECHO_RAW;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_var_t, var)
{
    sl_vm_insn_t insn;
    size_t frame;
    sl_compile_state_t* xcs = cs;
    size_t index;
    if(cs->parent) {
        frame = 0;
        while(xcs->parent) {
            if(st_lookup(xcs->vars, (st_data_t)node->name, (st_data_t*)&index)) {
                if(frame == 0) {
                    insn.opcode = SL_OP_MOV;
                    emit(cs, insn);
                } else {
                    insn.opcode = SL_OP_SET_OUTER;
                    emit(cs, insn);
                    insn.uint = frame;
                    emit(cs, insn);
                }    
                insn.uint = index;
                emit(cs, insn);
                insn.uint = dest;
                emit(cs, insn);
                return;
            }
            xcs = xcs->parent;
            frame++;
        }
    }
    insn.opcode = SL_OP_GET_GLOBAL;
    emit(cs, insn);
    insn.str = node->name;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_var_t, ivar)
{
    sl_vm_insn_t insn;
    insn.opcode = SL_OP_GET_IVAR;
    emit(cs, insn);
    insn.str = node->name;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_var_t, cvar)
{
    sl_vm_insn_t insn;
    insn.opcode = SL_OP_GET_CVAR;
    emit(cs, insn);
    insn.str = node->name;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_immediate_t, immediate)
{
    emit_immediate(cs, node->value, dest);
}

NODE(sl_node_base_t, self)
{
    sl_vm_insn_t insn;
    insn.opcode = SL_OP_SELF;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    (void)node;
}

/* @TODO: class, def, lambda, try */

NODE(sl_node_if_t, if)
{
    sl_vm_insn_t insn;
    size_t fixup;
    
    /* emit a jump over the true branch, keeping a pointer to fixup later */
    compile_node(cs, node->condition, dest);
    insn.opcode = SL_OP_JUMP_UNLESS;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup = emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    
    /* true branch */
    compile_node(cs, node->body, dest);
    
    /*  emit a jump over the else body, and compensate for the jump 
        by adding two bytes to the fixup's operand */
    cs->section->insns[fixup].uint = cs->section->insns_count + 2 /* JUMP <end> */;
    insn.opcode = SL_OP_JUMP;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup = emit(cs, insn);
    
    if(node->else_body) {
        compile_node(cs, node->else_body, dest);
    } else {
        emit_immediate(cs, cs->vm->lib.nil, dest);
    }
    
    cs->section->insns[fixup].uint = cs->section->insns_count;
}

NODE(sl_node_while_t, while)
{
    sl_vm_insn_t insn;
    size_t fixup, begin;
    next_last_frame_t nl;
    
    begin = cs->section->insns_count;
    
    /* loop condition */
    compile_node(cs, node->expr, dest);
    
    /* emit code for !condition: */
    insn.opcode = SL_OP_JUMP_UNLESS;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup = emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    
    /* push this loop on to the next/last fixup stack */
    nl.next_fixups = NULL;
    nl.last_fixups = NULL;
    nl.prev = cs->next_last_frames;
    cs->next_last_frames = &nl;
    
    /* loop body */
    compile_node(cs, node->body, dest);
    
    /* place the right address in the next fixups */
    cs->next_last_frames = nl.prev;
    while(nl.next_fixups) {
        cs->section->insns[nl.next_fixups->fixup].uint = begin;
        nl.next_fixups = nl.next_fixups->next;
    }
    
    /* jump back to condition */
    insn.opcode = SL_OP_JUMP;
    emit(cs, insn);
    insn.uint = begin;
    emit(cs, insn);
    
    /* put the current IP into the JUMP_UNLESS fixup */
    cs->section->insns[fixup].uint = cs->section->insns_count;
    
    /* place the right address in the last fixups */
    while(nl.last_fixups) {
        cs->section->insns[nl.last_fixups->fixup].uint = cs->section->insns_count;
        nl.last_fixups = nl.last_fixups->next;
    }
    
    emit_immediate(cs, cs->vm->lib.nil, dest);
}

NODE(sl_node_for_t, for)
{
    /* @TODO */
}

NODE(sl_node_send_t, send)
{
    sl_vm_insn_t insn;
    size_t arg_base, i;
    /* compile the receiver into our 'dest' register */
    compile_node(cs, node->recv, dest);
    arg_base = reg_alloc_block(cs, node->arg_count);
    for(i = 0; i < node->arg_count; i++) {
        compile_node(cs, node->args[i], arg_base + i);
    }
    insn.opcode = SL_OP_SEND;
    emit(cs, insn);
    insn.uint = dest; /* recv */
    emit(cs, insn);
    insn.imm = node->id;
    emit(cs, insn);
    insn.uint = arg_base;
    emit(cs, insn);
    insn.uint = node->arg_count;
    emit(cs, insn);
    insn.uint = dest; /* destination */
    emit(cs, insn);
    reg_free_block(cs, arg_base, node->arg_count);
}

NODE(sl_node_const_t, const)
{
    sl_vm_insn_t insn;
    if(node->obj) {
        compile_node(cs, node->obj, dest);
        insn.opcode = SL_OP_GET_OBJECT_CONST;
        emit(cs, insn);
        insn.uint = dest;
        emit(cs, insn);
        insn.imm = node->id;
        emit(cs, insn);
        insn.uint = dest;
        emit(cs, insn);
    } else {
        insn.opcode = SL_OP_GET_CONST;
        emit(cs, insn);
        insn.imm = node->id;
        emit(cs, insn);
        insn.uint = dest;
        emit(cs, insn);
    }
}

NODE(sl_node_binary_t, and)
{
    sl_vm_insn_t insn;
    size_t fixup;
    
    compile_node(cs, node->left, dest);
    
    insn.opcode = SL_OP_JUMP_UNLESS;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup = emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    
    compile_node(cs, node->right, dest);
    
    cs->section->insns[fixup].uint = cs->section->insns_count;
}

NODE(sl_node_binary_t, or)
{
    sl_vm_insn_t insn;
    size_t fixup;
    
    compile_node(cs, node->left, dest);
    
    insn.opcode = SL_OP_JUMP_IF;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup = emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    
    compile_node(cs, node->right, dest);
    
    cs->section->insns[fixup].uint = cs->section->insns_count;
}

NODE(sl_node_unary_t, not)
{
    sl_vm_insn_t insn;
    compile_node(cs, node->expr, dest);
    insn.opcode = SL_OP_NOT;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_unary_t, return)
{
    sl_vm_insn_t insn;
    compile_node(cs, node->expr, dest);
    insn.opcode = SL_OP_RETURN;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
}

NODE(sl_node_range_t, range)
{
    sl_vm_insn_t insn;
    size_t left = dest, right = reg_alloc(cs);
    compile_node(cs, node->left, left);
    compile_node(cs, node->right, right);
    if(node->exclusive) {
        insn.opcode = SL_OP_RANGE_EX;
    } else {
        insn.opcode = SL_OP_RANGE;
    }
    emit(cs, insn);
    insn.uint = left;
    emit(cs, insn);
    insn.uint = right;
    emit(cs, insn);
    insn.uint = dest;
    emit(cs, insn);
    reg_free(cs, right);
}

NODE(sl_node_base_t, next)
{
    sl_vm_insn_t insn;
    fixup_t* fixup = sl_alloc(cs->vm->arena, sizeof(fixup_t));
    fixup->next = cs->next_last_frames->next_fixups;
    cs->next_last_frames->next_fixups = fixup;
    insn.opcode = SL_OP_JUMP;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup->fixup = emit(cs, insn);
    
    (void)node;
    (void)dest;
}

NODE(sl_node_base_t, last)
{
    sl_vm_insn_t insn;
    fixup_t* fixup = sl_alloc(cs->vm->arena, sizeof(fixup_t));
    fixup->next = cs->next_last_frames->last_fixups;
    cs->next_last_frames->last_fixups = fixup;
    insn.opcode = SL_OP_JUMP;
    emit(cs, insn);
    insn.uint = 0x0000CAFE;
    fixup->fixup = emit(cs, insn);
    
    (void)node;
    (void)dest;
}

#define COMPILE(type, caps, name) case SL_NODE_##caps: compile_##name(cs, (type*)node, dest); return;

static void
compile_node(sl_compile_state_t* cs, sl_node_base_t* node, size_t dest)
{
    switch(node->type) {
        COMPILE(sl_node_seq_t,       SEQ,       seq);
        COMPILE(sl_node_raw_t,       RAW,       raw);
        COMPILE(sl_node_echo_t,      ECHO,      echo);
        COMPILE(sl_node_echo_t,      ECHO_RAW,  echo_raw);
        COMPILE(sl_node_var_t,       VAR,       var);
        COMPILE(sl_node_var_t,       IVAR,      ivar);
        COMPILE(sl_node_var_t,       CVAR,      cvar);
        COMPILE(sl_node_immediate_t, IMMEDIATE, immediate);
        COMPILE(sl_node_base_t,      SELF,      self);
        /*
        COMPILE(sl_node_class_t,     CLASS,     class);
        COMPILE(sl_node_def_t,       DEF,       def);
        COMPILE(sl_node_lambda_t,    LAMBDA,    lambda);
        COMPILE(sl_node_try_t,       TRY,       try);
        */
        COMPILE(sl_node_if_t,        IF,        if);
        COMPILE(sl_node_while_t,     WHILE,     while);
        COMPILE(sl_node_for_t,       FOR,       for);
        COMPILE(sl_node_send_t,      SEND,      send);
        COMPILE(sl_node_const_t,     CONST,     const);
        COMPILE(sl_node_binary_t,    AND,       and);
        COMPILE(sl_node_binary_t,    OR,        or);
        COMPILE(sl_node_unary_t,     NOT,       not);
        COMPILE(sl_node_unary_t,     RETURN,    return);
        COMPILE(sl_node_range_t,     RANGE,     range);
        COMPILE(sl_node_base_t,      NEXT,      next);
        COMPILE(sl_node_base_t,      LAST,      last);
    }
    sl_throw_message(cs->vm, "Unknown node type in compile_node");
}

sl_vm_section_t*
sl_compile(sl_vm_t* vm, sl_node_base_t* ast)
{
    sl_vm_insn_t insn;
    sl_compile_state_t cs;
    cs.vm = vm;
    cs.vars = st_init_table(vm->arena, &sl_string_hash_type);
    cs.parent = NULL;
    cs.section = sl_alloc(vm->arena, sizeof(sl_vm_section_t));
    cs.section->max_registers = 1;
    cs.section->insns_cap = 4;
    cs.section->insns_count = 0;
    cs.section->insns = sl_alloc(vm->arena, sizeof(sl_vm_insn_t) * cs.section->insns_cap);
    cs.registers = sl_alloc(vm->arena, cs.section->max_registers);
    cs.registers[0] = 1;
    cs.next_last_frames = NULL;
    compile_node(&cs, ast, 0);
    
    insn.opcode = SL_OP_IMMEDIATE;
    emit(&cs, insn);
    insn.imm = vm->lib.nil;
    emit(&cs, insn);
    insn.uint = 0;
    emit(&cs, insn);
    insn.opcode = SL_OP_RETURN;
    emit(&cs, insn);
    insn.uint = 0;
    emit(&cs, insn);
    
    return cs.section;
}