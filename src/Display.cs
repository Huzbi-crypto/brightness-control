using System;
using System.Collections.Generic;
using System.Management;
using System.Runtime.InteropServices;

namespace BrightnessControl
{
    internal sealed class DdcMonitor
    {
        public IntPtr Handle;
        public uint Max;
    }

    /// <summary>
    /// Hardware access: internal panel via WMI, external monitors via DDC/CI (VCP 0x10).
    /// Every member here is called from the single worker thread in TrayApp, never the UI thread.
    /// </summary>
    internal static class Display
    {
        private delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, IntPtr lprc, IntPtr data);

        [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);
        [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
        [DllImport("dxva2.dll", SetLastError = true)] private static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr h, ref uint n);

        // hPhysicalMonitor is the FIRST field of the native struct.
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct PHYSICAL_MONITOR
        {
            public IntPtr hPhysicalMonitor;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string szPhysicalMonitorDescription;
        }

        [DllImport("dxva2.dll", SetLastError = true)] private static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr h, uint size, [Out] PHYSICAL_MONITOR[] arr);
        [DllImport("dxva2.dll", SetLastError = true)] private static extern bool GetMonitorCapabilities(IntPtr h, ref uint caps, ref uint ct);
        [DllImport("dxva2.dll", SetLastError = true)] private static extern bool GetVCPFeatureAndVCPFeatureReply(IntPtr h, byte code, IntPtr type, ref uint cur, ref uint max);
        [DllImport("dxva2.dll", SetLastError = true)] private static extern bool SetVCPFeature(IntPtr h, byte code, uint val);

        private const byte VCP_BRIGHTNESS = 0x10;

        private static List<DdcMonitor> _externals = new List<DdcMonitor>();
        private static ManagementObject _internal;
        private static ManagementClass _methods;

        // ---------------- internal panel (WMI) ----------------

        public static int ReadInternal()
        {
            try
            {
                if (_internal == null)
                {
                    string inst = null;
                    using (ManagementObjectSearcher s = new ManagementObjectSearcher("root\\WMI", "SELECT InstanceName FROM WmiMonitorBrightness"))
                    using (ManagementObjectCollection r = s.Get())
                        foreach (ManagementBaseObject o in r) inst = o["InstanceName"] as string;
                    if (inst == null) return -1;
                    // Bind straight to the one instance (~2 ms) instead of re-running a WQL query (~4+ ms).
                    _internal = new ManagementObject("\\\\.\\root\\WMI:WmiMonitorBrightness.InstanceName=\"" + inst.Replace("\\", "\\\\") + "\"");
                    _internal.Get();
                }
                _internal.Get();
                object v = _internal["CurrentBrightness"];
                return v == null ? -1 : Convert.ToInt32(v);
            }
            catch
            {
                _internal = null;
                return -1;
            }
        }

        public static bool WriteInternal(int percent)
        {
            try
            {
                if (_methods == null)
                    _methods = new ManagementClass("root\\WMI", "WmiMonitorBrightnessMethods", null);
                byte val = (byte)Math.Max(0, Math.Min(100, percent));
                foreach (ManagementObject mo in _methods.GetInstances())
                {
                    using (mo)
                    {
                        ManagementBaseObject inParams = mo.GetMethodParameters("WmiSetBrightness");
                        inParams["Timeout"] = (uint)0;
                        inParams["Brightness"] = val;
                        mo.InvokeMethod("WmiSetBrightness", inParams, null);
                    }
                }
                return true;
            }
            catch { return false; }
        }

        // ---------------- external monitors (DDC/CI) ----------------

        private static void ProbeExternals()
        {
            List<DdcMonitor> found = new List<DdcMonitor>();
            try
            {
                EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr hMon, IntPtr hdc, IntPtr lprc, IntPtr data)
                {
                    uint n = 0;
                    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, ref n) && n > 0)
                    {
                        PHYSICAL_MONITOR[] arr = new PHYSICAL_MONITOR[n];
                        if (GetPhysicalMonitorsFromHMONITOR(hMon, n, arr))
                        {
                            foreach (PHYSICAL_MONITOR pm in arr)
                            {
                                uint caps = 0, ct = 0;
                                if (!GetMonitorCapabilities(pm.hPhysicalMonitor, ref caps, ref ct)) continue;
                                uint cur = 0, max = 0;
                                if (!GetVCPFeatureAndVCPFeatureReply(pm.hPhysicalMonitor, VCP_BRIGHTNESS, IntPtr.Zero, ref cur, ref max)) continue;
                                if (max == 0) continue;
                                DdcMonitor m = new DdcMonitor();
                                m.Handle = pm.hPhysicalMonitor;
                                m.Max = max;
                                found.Add(m);
                            }
                        }
                    }
                    return true;
                }, IntPtr.Zero);
            }
            catch { }
            _externals = found;
        }

        public static bool HasExternal
        {
            get { if (_externals.Count == 0) ProbeExternals(); return _externals.Count > 0; }
        }

        public static int ReadExternal()
        {
            if (_externals.Count == 0) ProbeExternals();
            foreach (DdcMonitor m in _externals)
            {
                uint cur = 0, max = 0;
                if (GetVCPFeatureAndVCPFeatureReply(m.Handle, VCP_BRIGHTNESS, IntPtr.Zero, ref cur, ref max) && max > 0)
                    return (int)Math.Round(cur * 100.0 / max);
            }
            return -1;
        }

        public static bool WriteExternal(int percent)
        {
            if (_externals.Count == 0) ProbeExternals();
            if (_externals.Count == 0) return false;

            int pct = Math.Max(0, Math.Min(100, percent));
            bool ok = true;
            foreach (DdcMonitor m in _externals)
            {
                uint target = (uint)Math.Round(pct / 100.0 * m.Max);
                if (!SetVCPFeature(m.Handle, VCP_BRIGHTNESS, target)) ok = false;
            }
            if (!ok)
            {
                // Stale handles (monitor slept, cable reseated, resolution change) -> re-enumerate once.
                ProbeExternals();
                foreach (DdcMonitor m in _externals)
                {
                    uint target = (uint)Math.Round(pct / 100.0 * m.Max);
                    SetVCPFeature(m.Handle, VCP_BRIGHTNESS, target);
                }
            }
            return true;
        }

        /// <summary>Model name of the external monitor, e.g. "DELL P2423D".</summary>
        public static string ExternalName()
        {
            try
            {
                string internalId = null;
                try
                {
                    using (ManagementObjectSearcher s = new ManagementObjectSearcher("root\\WMI", "SELECT InstanceName FROM WmiMonitorBrightness"))
                    using (ManagementObjectCollection r = s.Get())
                        foreach (ManagementBaseObject o in r)
                            if (o["InstanceName"] != null) internalId = o["InstanceName"].ToString();
                }
                catch { }

                using (ManagementObjectSearcher s = new ManagementObjectSearcher("root\\WMI", "SELECT * FROM WmiMonitorID"))
                using (ManagementObjectCollection r = s.Get())
                {
                    foreach (ManagementBaseObject o in r)
                    {
                        string inst = o["InstanceName"] as string;
                        if (internalId != null && inst == internalId) continue;
                        string name = Decode(o["UserFriendlyName"] as ushort[]);
                        string mfg = Decode(o["ManufacturerName"] as ushort[]);
                        if (string.IsNullOrEmpty(name) || name.StartsWith("ScreenXpert", StringComparison.OrdinalIgnoreCase)) continue;
                        if (mfg == "TSB") continue;
                        return name;
                    }
                }
            }
            catch { }
            return "External display";
        }

        private static string Decode(ushort[] raw)
        {
            if (raw == null) return "";
            char[] chars = new char[raw.Length];
            int n = 0;
            foreach (ushort c in raw)
            {
                if (c == 0) break;
                chars[n++] = (char)c;
            }
            return new string(chars, 0, n).Trim();
        }
    }
}
