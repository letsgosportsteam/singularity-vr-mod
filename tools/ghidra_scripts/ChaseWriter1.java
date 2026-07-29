// ChaseWriter1 - analyse the pitch writer Ghidra had not disassembled.
//
// The hardware breakpoint's FIRST hit in both runs was EIP 0x01034121, which Ghidra reported
// as "no containing function - JMP 0x010341ca". That is an unanalysed region rather than a
// dead end, and being the first writer it is more likely to be the real view-rotation setter
// than the generic AActor::SetRotation plumbing found at writer #2.
//
// Forces disassembly/function creation at both addresses, then decompiles and reports callers.
//
// Run:  .\tools\ghidra.ps1 -Script ChaseWriter1.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

public class ChaseWriter1 extends GhidraScript {

    private static final String[] TARGETS = { "01034121", "010341ca" };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\writer1.txt");

        for (String t : TARGETS) {
            Address a = addr(t);
            emit(out, "");
            emit(out, "==================================================");
            emit(out, " " + a);
            emit(out, "==================================================");

            Function f = getFunctionContaining(a);
            if (f == null) {
                emit(out, "  no function - forcing disassembly/creation");
                if (currentProgram.getListing().getInstructionContaining(a) == null) {
                    disassemble(a);
                }
                f = getFunctionContaining(a);
                if (f == null) {
                    try { f = createFunction(a, null); } catch (Exception e) { }
                }
                if (f == null) {
                    // walk backwards to find a function start
                    Address probe = a;
                    for (int i = 0; i < 4000 && f == null; i++) {
                        probe = probe.subtract(1);
                        f = getFunctionContaining(probe);
                    }
                    if (f != null) emit(out, "  found enclosing function by walking back");
                }
            }

            if (f == null) {
                emit(out, "  STILL no function. raw disassembly around it:");
                Address s = a.subtract(48);
                for (int i = 0; i < 40; i++) {
                    Instruction ins = currentProgram.getListing().getInstructionContaining(s);
                    if (ins == null) { s = s.add(1); continue; }
                    emit(out, (ins.getAddress().equals(a) ? "  >>>> " : "        ") + ins.getAddress() + "  " + ins);
                    s = ins.getAddress().add(ins.getLength());
                }
                continue;
            }

            emit(out, "  function " + f.getName() + " entry=" + f.getEntryPoint() +
                      " size=" + f.getBody().getNumAddresses() +
                      " offset +0x" + Long.toHexString(a.subtract(f.getEntryPoint())));

            emit(out, "  --- disassembly around the write ---");
            Instruction ins = currentProgram.getListing().getInstructionContaining(a);
            Instruction cur = ins;
            for (int i = 0; i < 16 && cur != null; i++) {
                Instruction p = cur.getPrevious();
                if (p == null || !f.getBody().contains(p.getAddress())) break;
                cur = p;
            }
            for (int i = 0; i < 34 && cur != null; i++) {
                emit(out, (cur.getAddress().equals(a) ? "  >>>> " : "        ") + cur.getAddress() + "  " + cur);
                cur = cur.getNext();
            }

            emit(out, "  --- callers ---");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            int n = 0;
            while (it.hasNext() && n < 15) {
                Reference r = it.next();
                Function cf = getFunctionContaining(r.getFromAddress());
                emit(out, "     " + r.getFromAddress() +
                          (cf != null ? ("  in " + cf.getName() + " size=" + cf.getBody().getNumAddresses()) : ""));
                n++;
            }
            if (n == 0) emit(out, "     (none direct)");

            emit(out, "  --- decompiled ---");
            if (f.getBody().getNumAddresses() <= 24000) {
                DecompInterface di = new DecompInterface();
                di.openProgram(currentProgram);
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                emit(out, (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : "  (failed)");
                di.dispose();
            } else emit(out, "  (too large)");
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\writer1.txt");
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
