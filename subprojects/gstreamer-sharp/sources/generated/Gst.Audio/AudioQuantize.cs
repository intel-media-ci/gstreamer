// This file was generated by the Gtk# code generator.
// Any changes made will be lost if regenerated.

namespace Gst.Audio {

	using System;
	using System.Collections;
	using System.Collections.Generic;
	using System.Runtime.InteropServices;

#region Autogenerated code
	public partial class AudioQuantize : GLib.Opaque {

		[DllImport("gstaudio-1.0-0.dll", CallingConvention = CallingConvention.Cdecl)]
		static extern void gst_audio_quantize_reset(IntPtr raw);

		public void Reset() {
			gst_audio_quantize_reset(Handle);
		}

		[DllImport("gstaudio-1.0-0.dll", CallingConvention = CallingConvention.Cdecl)]
		static extern void gst_audio_quantize_samples(IntPtr raw, IntPtr in_param, IntPtr out_param, uint samples);

		public void Samples(IntPtr in_param, IntPtr out_param, uint samples) {
			gst_audio_quantize_samples(Handle, in_param, out_param, samples);
		}

		public void Samples(uint samples) {
			Samples (IntPtr.Zero, IntPtr.Zero, samples);
		}

		public AudioQuantize(IntPtr raw) : base(raw) {}

		[DllImport("gstaudio-1.0-0.dll", CallingConvention = CallingConvention.Cdecl)]
		static extern void gst_audio_quantize_free(IntPtr raw);

		protected override void Free (IntPtr raw)
		{
			gst_audio_quantize_free (raw);
		}

		protected override Action<IntPtr> DisposeUnmanagedFunc {
			get {
				return gst_audio_quantize_free;
			}
		}


		// Internal representation of the wrapped structure ABI.
		static GLib.AbiStruct _abi_info = null;
		static public GLib.AbiStruct abi_info {
			get {
				if (_abi_info == null)
					_abi_info = new GLib.AbiStruct (new List<GLib.AbiField>{ 
					});

				return _abi_info;
			}
		}


		// End of the ABI representation.

#endregion
	}
}
