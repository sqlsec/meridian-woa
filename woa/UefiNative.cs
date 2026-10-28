// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 meridian-woa contributors
// Native Windows UEFI access. No firmware calls occur merely by loading this type.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace MeridianBoot {
    public sealed class FirmwareValue {
        public byte[] Data;
        public uint Attributes;
        public FirmwareValue(byte[] data, uint attributes) { Data = data; Attributes = attributes; }
    }
    public static class Native {
        private const string GlobalGuid = "{8BE4DF61-93CA-11d2-AA0D-00E098032B8C}";
        [StructLayout(LayoutKind.Sequential)] private struct Luid { public uint Low; public int High; }
        [StructLayout(LayoutKind.Sequential)] private struct TokenPrivileges { public uint Count; public Luid Luid; public uint Attributes; }
        [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError=true)] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")] private static extern void SetLastError(uint error);
        [DllImport("advapi32.dll", SetLastError=true)] private static extern bool OpenProcessToken(IntPtr process, uint desired, out IntPtr token);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern bool LookupPrivilegeValue(string system, string name, out Luid luid);
        [DllImport("advapi32.dll", SetLastError=true)] private static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state, uint size, IntPtr previous, IntPtr returnSize);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern uint GetFirmwareEnvironmentVariableEx(string name, string guid, byte[] buffer, uint size, out uint attributes);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] private static extern bool SetFirmwareEnvironmentVariableEx(string name, string guid, byte[] buffer, uint size, uint attributes);

        public static void EnablePrivilege() {
            IntPtr token;
            if (!OpenProcessToken(GetCurrentProcess(), 0x20 | 0x8, out token)) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                Luid luid;
                if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid)) throw new Win32Exception(Marshal.GetLastWin32Error());
                TokenPrivileges state = new TokenPrivileges { Count=1, Luid=luid, Attributes=2 };
                SetLastError(0);
                if (!AdjustTokenPrivileges(token, false, ref state, 0, IntPtr.Zero, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
                int error = Marshal.GetLastWin32Error();
                if (error != 0) throw new Win32Exception(error);
            } finally { CloseHandle(token); }
        }
        public static FirmwareValue Read(string name) {
            // SecureBoot/BitLocker PowerShell cmdlets can restore this process privilege
            // to disabled. Acquire it immediately before every firmware operation.
            EnablePrivilege();
            for (int size=4096; size<=1024*1024; size*=2) {
                byte[] buffer = new byte[size];
                uint attributes;
                uint length = GetFirmwareEnvironmentVariableEx(name, GlobalGuid, buffer, (uint)buffer.Length, out attributes);
                if (length == 0) {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 203) return null; // ERROR_ENVVAR_NOT_FOUND only
                    if (error == 122) continue; // ERROR_INSUFFICIENT_BUFFER
                    throw new Win32Exception(error, "Reading UEFI variable " + name + "; Win32=" + error + " (" + new Win32Exception(error).Message + "), buffer=" + size);
                }
                Array.Resize(ref buffer, checked((int)length));
                return new FirmwareValue(buffer, attributes);
            }
            throw new IOException("UEFI variable exceeds maximum 1 MiB read buffer: " + name);
        }
        public static bool Equal(byte[] a, byte[] b) {
            if (a == null || b == null) return a == b;
            if (a.Length != b.Length) return false;
            for (int i=0; i<a.Length; ++i) if(a[i] != b[i]) return false;
            return true;
        }
        public static void Write(string name, byte[] data, uint attributes) {
            if (data == null || data.Length == 0) throw new ArgumentException("Use Delete for deleting variables.");
            EnablePrivilege();
            if (!SetFirmwareEnvironmentVariableEx(name, GlobalGuid, data, (uint)data.Length, attributes)) { int error=Marshal.GetLastWin32Error(); throw new Win32Exception(error, "Writing UEFI variable " + name + "; Win32=" + error + " (" + new Win32Exception(error).Message + ")"); }
            FirmwareValue readback = Read(name);
            if (readback == null || !Equal(data, readback.Data) || readback.Attributes != attributes) throw new IOException("UEFI readback mismatch: " + name);
        }
        public static void Delete(string name) {
            FirmwareValue current = Read(name);
            if (current == null) return;
            EnablePrivilege();
            if (!SetFirmwareEnvironmentVariableEx(name, GlobalGuid, null, 0, current.Attributes)) { int error=Marshal.GetLastWin32Error(); throw new Win32Exception(error, "Deleting UEFI variable " + name + "; Win32=" + error + " (" + new Win32Exception(error).Message + ")"); }
            if (Read(name) != null) throw new IOException("UEFI deletion readback mismatch: " + name);
        }
        public static ushort[] DecodeOrder(byte[] data) {
            if (data == null || (data.Length % 2) != 0) throw new ArgumentException("BootOrder must contain UINT16 values.");
            ushort[] result = new ushort[data.Length/2];
            for(int i=0;i<result.Length;++i) result[i]=BitConverter.ToUInt16(data,2*i);
            return result;
        }
        public static byte[] EncodeOrder(ushort[] order) {
            using(MemoryStream stream = new MemoryStream()) using(BinaryWriter writer = new BinaryWriter(stream)) {
                foreach(ushort entry in order) writer.Write(entry);
                return stream.ToArray();
            }
        }
        public static byte[] BuildLoadOption(uint partitionNumber, ulong startLba, ulong sizeLba, Guid partitionGuid, string path, string description) {
            if (partitionNumber == 0 || sizeLba == 0 || partitionGuid == Guid.Empty) throw new ArgumentException("Invalid GPT partition.");
            if (String.IsNullOrEmpty(path) || path[0] != '\\' || path.IndexOf('\0') >= 0 || path.IndexOf("..", StringComparison.Ordinal) >= 0) throw new ArgumentException("Use an absolute EFI path.");
            if (String.IsNullOrEmpty(description) || description.IndexOf('\0') >= 0) throw new ArgumentException("Invalid description.");
            byte[] pathBytes = Encoding.Unicode.GetBytes(path + "\0");
            byte[] devicePath;
            using(MemoryStream stream = new MemoryStream()) using(BinaryWriter writer = new BinaryWriter(stream)) {
                writer.Write((byte)4); writer.Write((byte)1); writer.Write((ushort)42);
                writer.Write(partitionNumber); writer.Write(startLba); writer.Write(sizeLba); writer.Write(partitionGuid.ToByteArray());
                writer.Write((byte)2); writer.Write((byte)2); // GPT, GUID signature
                writer.Write((byte)4); writer.Write((byte)4); writer.Write(checked((ushort)(4 + pathBytes.Length))); writer.Write(pathBytes);
                writer.Write((byte)0x7f); writer.Write((byte)0xff); writer.Write((ushort)4);
                devicePath = stream.ToArray();
            }
            using(MemoryStream stream = new MemoryStream()) using(BinaryWriter writer = new BinaryWriter(stream)) {
                writer.Write((uint)1); // LOAD_OPTION_ACTIVE
                writer.Write(checked((ushort)devicePath.Length));
                writer.Write(Encoding.Unicode.GetBytes(description + "\0")); writer.Write(devicePath);
                return stream.ToArray();
            }
        }
        public static void ValidateArm64Efi(string path) {
            using(FileStream stream = File.OpenRead(path)) using(BinaryReader reader = new BinaryReader(stream)) {
                if(stream.Length < 128 || reader.ReadUInt16() != 0x5a4d) throw new InvalidDataException("Missing MZ header: " + path);
                stream.Position=0x3c; uint pe = reader.ReadUInt32();
                if(pe > stream.Length - 96) throw new InvalidDataException("Invalid PE offset: " + path);
                stream.Position=pe;
                if(reader.ReadUInt32()!=0x4550 || reader.ReadUInt16()!=0xaa64) throw new InvalidDataException("Expected ARM64 PE executable: " + path);
                stream.Position=pe+20; ushort optionalSize=reader.ReadUInt16();
                if(optionalSize < 70) throw new InvalidDataException("Truncated PE optional header: " + path);
                stream.Position=pe+24;
                if(reader.ReadUInt16()!=0x20b) throw new InvalidDataException("Expected PE32+ executable: " + path);
                stream.Position=pe+24+68;
                if(reader.ReadUInt16()!=10) throw new InvalidDataException("Expected EFI application subsystem 10: " + path);
            }
        }
    }
}
