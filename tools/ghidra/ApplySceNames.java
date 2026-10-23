// SPDX-License-Identifier: MIT
//
// Names the Sony SDK and libc functions that PS2Recomp's ps2_analyzer
// recognises from its SCE symbol database (sceSifInitRpc, scePadRead,
// memcpy, ...). The ELF is stripped, so without this the exporter sees only
// FUN_ names, nothing is replaced by the runtime's handlers, and the game's
// own SIF, CD and pad libraries run against hardware that is not there.
//
// Arguments: the TOML ps2_analyzer wrote, then any hand-made list of names
// (recomp/config/SCUS-97133.names.toml). Every "name@0xADDRESS" string in
// them is applied. Hand-made names win over the database's; names given in
// the Ghidra project itself are kept for database matches. A missing function
// is created when the address is not inside another one.
//
// Run by tools/scripts/analyze-ghidra.ps1 before ExportPS2Functions.java.
//@category PS2

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.exception.DuplicateNameException;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class ApplySceNames extends GhidraScript {
    private static final Pattern ENTRY = Pattern.compile("\"([A-Za-z_][A-Za-z0-9_]*)@0x([0-9A-Fa-f]{1,8})\"");

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("Usage: ApplySceNames.java <ps2_analyzer output toml> [more name lists]");
            return;
        }

        int[] counts = new int[3]; // renamed, created, kept
        for (int i = 0; i < args.length; ++i) {
            apply(Files.readString(Path.of(args[i])), i > 0, counts);
        }
        println("ApplySceNames: " + counts[0] + " functions renamed, " + counts[1] + " created, " + counts[2] +
                " already named and kept");
    }

    private void apply(String text, boolean byHand, int[] counts) throws Exception {
        int renamed = 0;
        int created = 0;
        int kept = 0;
        Matcher matcher = ENTRY.matcher(text);
        while (matcher.find() && !monitor.isCancelled()) {
            String name = matcher.group(1);
            if (name.startsWith("sub_")) {
                continue;
            }
            Address address = toAddr(Long.parseLong(matcher.group(2), 16));

            Function function = getFunctionAt(address);
            if (function == null) {
                if (getFunctionContaining(address) != null) {
                    continue;
                }
                disassemble(address);
                function = createFunction(address, null);
                if (function != null) {
                    rename(function, name);
                    ++created;
                }
                continue;
            }

            if (function.getName().equals(name)) {
                continue;
            }
            if (!byHand && function.getSymbol().getSource() != SourceType.DEFAULT) {
                ++kept;
                continue;
            }
            rename(function, name);
            ++renamed;
        }

        counts[0] += renamed;
        counts[1] += created;
        counts[2] += kept;
    }

    // A few library names occur twice (static helpers in two objects); the
    // second one gets its address appended.
    private void rename(Function function, String name) throws Exception {
        try {
            function.setName(name, SourceType.IMPORTED);
        } catch (DuplicateNameException e) {
            function.setName(String.format("%s_%08x", name, function.getEntryPoint().getOffset()), SourceType.IMPORTED);
        }
    }
}
