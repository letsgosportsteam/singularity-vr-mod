// FindScriptVM - locate UObject::ProcessInternal and UObject::ProcessEvent.
//
// The camera functions have no hookable addresses (see ENGINE_NOTES) because UE3 dispatches
// UnrealScript dynamically. The hook point is therefore ProcessEvent, the single chokepoint
// every script call passes through.
//
// Anchors: the script VM's own diagnostic strings survive in .rdata. "Unknown code token"
// sits in the bytecode dispatch default case, i.e. inside ProcessInternal. ProcessEvent
// calls ProcessInternal, so enumerating callers of that function should surface it.
//
// Run:  .\tools\ghidra.ps1 -Script FindScriptVM.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class FindScriptVM extends GhidraScript {

    private static final String[] ANCHORS = {
        "Unknown code token",   // ProcessInternal opcode dispatch default -> strongest
        "Script call stack",    // script stack dump, called from error paths
        "Accessed None",        // null-property access in the interpreter
        "ScriptWarning",
    };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\script_vm.txt");
        Memory mem = currentProgram.getMemory();

        Set<Function> vmCandidates = new LinkedHashSet<>();

        for (String anchor : ANCHORS) {
            emit(out, "");
            emit(out, "=== " + anchor + " ===");
            List<Address> hits = findUtf16(mem, anchor);
            if (hits.isEmpty()) { emit(out, "  (not found)"); continue; }

            for (Address h : hits) {
                emit(out, "  string @ " + h);
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(h);
                boolean any = false;
                while (it.hasNext()) {
                    Reference r = it.next();
                    any = true;
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) {
                        emit(out, "    xref " + r.getFromAddress() + " in " + f.getName() +
                                  "  entry=" + f.getEntryPoint() +
                                  "  size=" + f.getBody().getNumAddresses());
                        vmCandidates.add(f);
                    } else {
                        emit(out, "    xref " + r.getFromAddress() + " (no function)");
                    }
                }
                if (!any) emit(out, "    (no references)");
            }
        }

        // For each VM function found, list who calls it. ProcessEvent should appear as a
        // caller of ProcessInternal.
        emit(out, "");
        emit(out, "==================================================");
        emit(out, " callers of each script-VM candidate");
        emit(out, "==================================================");
        for (Function f : vmCandidates) {
            emit(out, "");
            emit(out, "--- callers of " + f.getName() + " (entry " + f.getEntryPoint() +
                      ", size " + f.getBody().getNumAddresses() + ") ---");
            ReferenceIterator it = currentProgram.getReferenceManager()
                                     .getReferencesTo(f.getEntryPoint());
            int n = 0;
            Set<String> seen = new LinkedHashSet<>();
            while (it.hasNext() && n < 30) {
                Reference r = it.next();
                Function cf = getFunctionContaining(r.getFromAddress());
                String line = (cf != null)
                    ? "  " + cf.getName() + "  entry=" + cf.getEntryPoint() +
                      "  size=" + cf.getBody().getNumAddresses() +
                      "  params=" + cf.getParameterCount()
                    : "  (indirect from " + r.getFromAddress() + ")";
                if (seen.add(line)) { emit(out, line); n++; }
            }
            if (n == 0) emit(out, "  (none - called only indirectly, e.g. via vtable)");
        }

        emit(out, "");
        emit(out, "ProcessEvent signature to look for: (UFunction* Function, void* Parms,");
        emit(out, "void* Result) as a __thiscall - so 3 explicit params plus ECX=this.");

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\script_vm.txt");
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private List<Address> findUtf16(Memory mem, String s) throws Exception {
        byte[] pat = new byte[s.length() * 2];
        for (int i = 0; i < s.length(); i++) { pat[i*2] = (byte) s.charAt(i); pat[i*2+1] = 0; }
        List<Address> out = new ArrayList<>();
        for (MemoryBlock blk : mem.getBlocks()) {
            if (!blk.isInitialized()) continue;
            Address at = blk.getStart();
            while (at != null && at.compareTo(blk.getEnd()) < 0) {
                if (monitor.isCancelled()) return out;
                Address f = mem.findBytes(at, blk.getEnd(), pat, null, true, monitor);
                if (f == null) break;
                out.add(f);
                at = f.add(2);
                if (out.size() > 40) return out;
            }
        }
        return out;
    }
}
