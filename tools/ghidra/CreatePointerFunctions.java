// SPDX-License-Identifier: MIT
//
// Creates the functions Ghidra's auto-analysis misses because nothing calls
// them directly: C++ virtual methods and callbacks whose only reference is a
// pointer in .data or .rodata. Ghidra leaves their bytes undefined, so the
// exporter never sees them and the recompiled game stops when it first calls
// one through its vtable.
//
// A data word becomes a function when it is 4-byte aligned, points into an
// executable block, is not inside an existing function, and the instruction
// pair before it is "jr $ra; <delay slot>" or the word before it is a nop
// (padding between functions). Functions called by the new code are created
// the same way, until nothing new is found.
//
// Run by tools/scripts/analyze-ghidra.ps1 before ExportPS2Functions.java.
//@category PS2

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.SourceType;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.LinkedHashSet;
import java.util.Set;

public class CreatePointerFunctions extends GhidraScript {
    private static final int JR_RA = 0x03e00008;

    private Memory memory;

    @Override
    protected void run() throws Exception {
        memory = currentProgram.getMemory();

        Set<Address> pointed = new LinkedHashSet<>();
        for (MemoryBlock block : memory.getBlocks()) {
            if (block.isExecute() || !block.isInitialized() || block.isOverlay()) {
                continue;
            }
            Address start = block.getStart();
            long size = block.getSize();
            for (long offset = (4 - start.getOffset() % 4) % 4; offset + 4 <= size; offset += 4) {
                Address target = toCode(memory.getInt(start.add(offset)));
                if (target != null) {
                    pointed.add(target);
                }
            }
        }

        Deque<Address> work = new ArrayDeque<>(pointed);
        int created = 0;
        while (!work.isEmpty() && !monitor.isCancelled()) {
            Address entry = work.removeFirst();
            if (!isMissedFunctionStart(entry)) {
                continue;
            }
            if (currentProgram.getListing().getInstructionAt(entry) == null) {
                new DisassembleCommand(entry, null, true).applyTo(currentProgram, monitor);
            }
            if (!new CreateFunctionCmd(entry).applyTo(currentProgram, monitor)) {
                continue;
            }
            Function function = getFunctionAt(entry);
            if (function == null) {
                continue;
            }
            ++created;

            InstructionIterator it = currentProgram.getListing().getInstructions(function.getBody(), true);
            while (it.hasNext()) {
                Instruction instruction = it.next();
                FlowType flow = instruction.getFlowType();
                if (!flow.isCall()) {
                    continue;
                }
                for (Address callee : instruction.getFlows()) {
                    work.addLast(callee);
                }
            }
        }

        println("CreatePointerFunctions: " + pointed.size() + " code pointers in data, " + created +
                " functions created, " + markLandingPads() + " exception landing pads marked");
    }

    // The old GCC exception table is (start, end, handler) triples ended by
    // -1, -1. The unwinder jumps to the handler, a landing pad in the middle of
    // a function, so each one gets a label and a call reference; the exporter
    // then lists it as an entry point the recompiled code can jump to.
    private int markLandingPads() throws Exception {
        MemoryBlock table = memory.getBlock(".gcc_except_table");
        if (table == null) {
            return 0;
        }
        int marked = 0;
        Address start = table.getStart();
        for (long offset = 0; offset + 12 <= table.getSize(); offset += 12) {
            int first = memory.getInt(start.add(offset));
            if (first == -1) {
                offset -= 4; // the terminator is two words, not three
                continue;
            }
            Address entry = start.add(offset + 8);
            Address handler = toCode(memory.getInt(entry));
            if (handler == null) {
                continue;
            }
            if (currentProgram.getListing().getInstructionAt(handler) == null) {
                new DisassembleCommand(handler, null, true).applyTo(currentProgram, monitor);
            }
            createLabel(handler, String.format("eh_landing_%08x", handler.getOffset()), true);
            currentProgram.getReferenceManager().addMemoryReference(entry, handler, RefType.COMPUTED_CALL,
                                                                    SourceType.USER_DEFINED, 0);
            ++marked;
        }
        return marked;
    }

    // A pointer value that lands on an aligned address in an executable block.
    private Address toCode(int value) {
        long offset = value & 0xffffffffL;
        if (offset % 4 != 0) {
            return null;
        }
        Address address;
        try {
            address = toAddr(offset);
        } catch (Exception e) {
            return null;
        }
        MemoryBlock block = memory.getBlock(address);
        return block != null && block.isExecute() && block.isInitialized() ? address : null;
    }

    private boolean isMissedFunctionStart(Address entry) {
        if (getFunctionContaining(entry) != null) {
            return false;
        }
        try {
            if (memory.getInt(entry.subtract(8)) == JR_RA) {
                return true;
            }
            return memory.getInt(entry.subtract(4)) == 0;
        } catch (MemoryAccessException | RuntimeException e) {
            return false;
        }
    }
}
