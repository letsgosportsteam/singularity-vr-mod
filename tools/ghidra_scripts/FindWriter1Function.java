// FindWriter1Function - recover the function containing the primary pitch writer.
//
// The HW breakpoint reported EIP 0x01034121. Data breakpoints trap AFTER the storing
// instruction, so the actual write is just before that address. Ghidra never analysed the
// region - consistent with a virtual function reached only through a vtable.
//
// Scan backwards for a standard MSVC prologue (PUSH EBP; MOV EBP,ESP), create the function,
// then decompile it and find which vtable slots point at it.
//
// Run:  .\tools\ghidra.ps1 -Script FindWriter1Function.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.io.PrintWriter;

public class FindWriter1Function extends GhidraScript {

    private static final long HIT = 0x01034121L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\writer1_func.txt");
        Memory mem = currentProgram.getMemory();

        // find the nearest preceding 55 8B EC prologue
        Address start = null;
        for (long back = 4; back < 0x3000; back++) {
            Address p = addr(HIT - back);
            try {
                if ((mem.getByte(p) & 0xFF) == 0x55 &&
                    (mem.getByte(p.add(1)) & 0xFF) == 0x8B &&
                    (mem.getByte(p.add(2)) & 0xFF) == 0xEC) { start = p; break; }
            } catch (Exception e) { break; }
        }
        if (start == null) { emit(out, "no prologue found"); out.close(); return; }
        emit(out, "candidate function start: " + start + "  (" + (HIT - start.getOffset()) + " bytes before the hit)");

        Function f = getFunctionAt(start);
        if (f == null) {
            if (currentProgram.getListing().getInstructionAt(start) == null) disassemble(start);
            try { f = createFunction(start, null); } catch (Exception e) { }
        }
        if (f == null) { emit(out, "could not create a function there"); out.close(); return; }
        emit(out, "function " + f.getName() + " size=" + f.getBody().getNumAddresses());

        // which vtables point at it? that identifies the class and slot.
        emit(out, "");
        emit(out, "=== vtable slots referencing this function ===");
        long target = start.getOffset();
        int found = 0;
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || !b.getName().contains("rdata")) continue;
            Address a = b.getStart();
            while (a.compareTo(b.getEnd()) < 0 && found < 20) {
                try {
                    long v = mem.getInt(a) & 0xFFFFFFFFL;
                    if (v == target) { emit(out, "   referenced from " + a); found++; }
                } catch (Exception e) { }
                a = a.add(4);
                if (monitor.isCancelled()) break;
            }
        }
        if (found == 0) emit(out, "   (none in .rdata - may be called through a computed address)");

        // the instructions immediately before the trap address contain the actual store
        emit(out, "");
        emit(out, "=== instructions leading up to the trap ===");
        Instruction ins = currentProgram.getListing().getInstructionAt(start);
        Instruction prev = null;
        while (ins != null && ins.getAddress().getOffset() <= HIT) {
            if (ins.getAddress().getOffset() >= HIT - 60)
                emit(out, (ins.getAddress().getOffset() == HIT ? "  >>>> " : "        ")
                          + ins.getAddress() + "  " + ins);
            prev = ins;
            ins = ins.getNext();
        }
        emit(out, "  (the storing instruction is the one immediately above >>>>)");

        emit(out, "");
        emit(out, "=== decompiled ===");
        if (f.getBody().getNumAddresses() <= 24000) {
            DecompInterface di = new DecompInterface();
            di.openProgram(currentProgram);
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            emit(out, (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : "(failed)");
            di.dispose();
        } else emit(out, "(too large)");

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\writer1_func.txt");
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(long v) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
    }
}
