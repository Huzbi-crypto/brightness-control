using System;
using System.Runtime.InteropServices;
class S {
  public struct PM { public IntPtr h; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string d; }
  static void Main() { Console.WriteLine(Marshal.SizeOf(typeof(PM))); }
}
