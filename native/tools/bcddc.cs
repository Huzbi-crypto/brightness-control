using System;
using System.Runtime.InteropServices;

static class Program
{
    public struct PHYSICAL_MONITOR
    {
        public IntPtr hPhysicalMonitor;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string desc;
    }

    public delegate bool MonitorEnumProc(IntPtr hMon, IntPtr hdc, IntPtr rect, IntPtr data);

    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);
    [DllImport("dxva2.dll", SetLastError = true)] public static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr h, ref uint n);
    [DllImport("dxva2.dll", SetLastError = true)] public static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr h, uint n, [Out] PHYSICAL_MONITOR[] arr);
    [DllImport("dxva2.dll", SetLastError = true)] public static extern bool GetVCPFeatureAndVCPFeatureReply(IntPtr h, byte code, IntPtr type, ref uint cur, ref uint max);
    [DllImport("dxva2.dll", SetLastError = true)] public static extern bool SetVCPFeature(IntPtr h, byte code, uint value);

    static IntPtr found = IntPtr.Zero;
    static uint foundMax = 100;

    static bool Find(IntPtr hMon, IntPtr hdc, IntPtr rect, IntPtr data)
    {
        if (found != IntPtr.Zero) return false;
        uint n = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, ref n) || n == 0) return true;
        var arr = new PHYSICAL_MONITOR[n];
        if (!GetPhysicalMonitorsFromHMONITOR(hMon, n, arr)) return true;
        foreach (var pm in arr)
        {
            Console.Error.WriteLine("  handle=" + pm.hPhysicalMonitor + " desc='" + pm.desc + "'");
            if (pm.hPhysicalMonitor == IntPtr.Zero) continue;
            uint cur = 0, max = 0;
            if (GetVCPFeatureAndVCPFeatureReply(pm.hPhysicalMonitor, 0x10, IntPtr.Zero, ref cur, ref max) && max > 0)
            {
                found = pm.hPhysicalMonitor;
                foundMax = max;
                return false;
            }
        }
        return true;
    }

    static int Main(string[] args)
    {
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, Find, IntPtr.Zero);
        if (found == IntPtr.Zero) { Console.Error.WriteLine("no DDC monitor"); return 1; }

        if (args.Length >= 2 && args[0] == "--set")
        {
            return SetVCPFeature(found, 0x10, uint.Parse(args[1])) ? 0 : 1;
        }
        uint cur = 0, max = 0;
        if (!GetVCPFeatureAndVCPFeatureReply(found, 0x10, IntPtr.Zero, ref cur, ref max)) return 1;
        Console.WriteLine(cur * 100 / max);
        return 0;
    }
}
