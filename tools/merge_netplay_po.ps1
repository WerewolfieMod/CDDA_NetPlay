param(
    [Parameter(Mandatory = $true)][string]$Po,
    [Parameter(Mandatory = $true)][string]$Mo,
    [switch]$Backup
)
# Merge msgid/msgstr pairs from a .po into an existing gettext .mo catalogue.
# The whole merge is done in C#: PowerShell unrolls returned Dictionary objects,
# which made an earlier PowerShell version write only the header.
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public sealed class Utf8Order : IComparer<string> {
    public int Compare(string a, string b) {
        byte[] x = Encoding.UTF8.GetBytes(a);
        byte[] y = Encoding.UTF8.GetBytes(b);
        int n = Math.Min(x.Length, y.Length);
        for (int i = 0; i < n; i++) { if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1; }
        return x.Length.CompareTo(y.Length);
    }
}

public static class MoMerge {
    static string Unescape(string s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.Length; i++) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.Length) {
                i++;
                switch (s[i]) {
                    case 'n': sb.Append('\n'); break;
                    case 't': sb.Append('\t'); break;
                    case 'r': sb.Append('\r'); break;
                    case '"': sb.Append('"'); break;
                    case '\\': sb.Append('\\'); break;
                    default: sb.Append(s[i]); break;
                }
            } else sb.Append(c);
        }
        return sb.ToString();
    }

    static Dictionary<string, string> ReadPo(string path) {
        Dictionary<string, string> map = new Dictionary<string, string>(StringComparer.Ordinal);
        string cur = "";
        string id = null, str = null;
        foreach (string raw in File.ReadAllLines(path, Encoding.UTF8)) {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith("#")) continue;
            if (line.StartsWith("msgid ")) {
                if (id != null && str != null) map[id] = str;
                id = Unescape(line.Substring(6).Trim('"'));
                str = null; cur = "id";
            } else if (line.StartsWith("msgstr ")) {
                str = Unescape(line.Substring(7).Trim('"'));
                cur = "str";
            } else if (line.StartsWith("\"")) {
                string piece = Unescape(line.Trim('"'));
                if (cur == "id") id += piece; else if (cur == "str") str += piece;
            }
        }
        if (id != null && str != null) map[id] = str;
        return map;
    }

    static Dictionary<string, string> ReadMo(string path) {
        byte[] b = File.ReadAllBytes(path);
        if (BitConverter.ToUInt32(b, 0) != 2500072158u) throw new Exception("not a little-endian .mo: " + path);
        int n = BitConverter.ToInt32(b, 8);
        int oo = BitConverter.ToInt32(b, 12);
        int ot = BitConverter.ToInt32(b, 16);
        Dictionary<string, string> map = new Dictionary<string, string>(StringComparer.Ordinal);
        for (int i = 0; i < n; i++) {
            int ll = BitConverter.ToInt32(b, oo + i * 8);
            int lo = BitConverter.ToInt32(b, oo + i * 8 + 4);
            int tl = BitConverter.ToInt32(b, ot + i * 8);
            int to = BitConverter.ToInt32(b, ot + i * 8 + 4);
            map[Encoding.UTF8.GetString(b, lo, ll)] = Encoding.UTF8.GetString(b, to, tl);
        }
        return map;
    }

    public static string Run(string poPath, string moPath, bool backup) {
        Dictionary<string, string> map = ReadMo(moPath);
        int before = map.Count;
        foreach (KeyValuePair<string, string> kv in ReadPo(poPath)) map[kv.Key] = kv.Value;
        List<string> keys = new List<string>(map.Keys);
        keys.Sort(new Utf8Order());
        int n = keys.Count;
        List<byte> ob = new List<byte>();
        List<byte> tb = new List<byte>();
        int[] oLen = new int[n], oOff = new int[n], tLen = new int[n], tOff = new int[n];
        Encoding enc = Encoding.UTF8;
        for (int i = 0; i < n; i++) {
            byte[] kb = enc.GetBytes(keys[i]);
            byte[] vb = enc.GetBytes(map[keys[i]]);
            oLen[i] = kb.Length; oOff[i] = ob.Count;
            ob.AddRange(kb); ob.Add(0);
            tLen[i] = vb.Length; tOff[i] = tb.Count;
            tb.AddRange(vb); tb.Add(0);
        }
        int stringsStart = 28 + n * 16;
        using (MemoryStream ms = new MemoryStream()) {
            BinaryWriter w = new BinaryWriter(ms);
            w.Write(2500072158u); w.Write(0u); w.Write((uint)n);
            w.Write(28u); w.Write((uint)(28 + n * 8));
            w.Write(0u); w.Write(0u);
            for (int i = 0; i < n; i++) { w.Write((uint)oLen[i]); w.Write((uint)(stringsStart + oOff[i])); }
            for (int i = 0; i < n; i++) { w.Write((uint)tLen[i]); w.Write((uint)(stringsStart + ob.Count + tOff[i])); }
            w.Write(ob.ToArray()); w.Write(tb.ToArray());
            w.Flush();
            if (backup && File.Exists(moPath)) File.Copy(moPath, moPath + ".bak", true);
            File.WriteAllBytes(moPath, ms.ToArray());
        }
        // verify
        Dictionary<string, string> check = ReadMo(moPath);
        int bad = 0;
        foreach (KeyValuePair<string, string> kv in ReadPo(poPath)) {
            string v;
            if (!check.TryGetValue(kv.Key, out v) || v != kv.Value) bad++;
        }
        return "po=" + ReadPo(poPath).Count + " before=" + before + " after=" + n +
               " readback=" + check.Count + " mismatches=" + bad;
    }
}
"@

$res = [MoMerge]::Run($Po, $Mo, $Backup.IsPresent)
Write-Output $res
if ($res -like '*mismatches=0') { Write-Output 'OK' } else { exit 1 }
