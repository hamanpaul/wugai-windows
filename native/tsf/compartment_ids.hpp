#ifndef BOSHIAMY_COMPARTMENT_IDS_HPP
#define BOSHIAMY_COMPARTMENT_IDS_HPP
#include <windows.h>
namespace boshiamy {
// This MinGW SDK omits the newer INPUTMODE_CONVERSION declaration.
// Microsoft windows-rs 0.58.0, Windows/Win32/UI/TextServices/mod.rs defines
// GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION with this exact value.
inline constexpr GUID kConversionCompartment={0xccf05dd8,0x4a87,0x11d7,{0xa6,0xe2,0x00,0x06,0x5b,0x84,0x43,0x5c}};
inline constexpr DWORD kNativeMask=0x0001;
inline constexpr DWORD kFullshapeMask=0x0008;
}
#endif
