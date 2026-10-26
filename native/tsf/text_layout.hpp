#ifndef LIUKAI_TEXT_LAYOUT_HPP
#define LIUKAI_TEXT_LAYOUT_HPP
#include <msctf.h>
// MinGW's current msctf.h omits this interface; match the Microsoft SDK ABI.
#ifndef __ITfTextLayoutSink_INTERFACE_DEFINED__
#define __ITfTextLayoutSink_INTERFACE_DEFINED__
enum TfLayoutCode {TF_LC_CREATE=0,TF_LC_CHANGE=1,TF_LC_DESTROY=2};
MIDL_INTERFACE("2af2d06a-dd5b-4927-a0b4-54f19c91fade")
ITfTextLayoutSink: public IUnknown {
 virtual HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext*,TfLayoutCode,ITfContextView*)=0;
};
#endif
namespace boshiamy {
inline constexpr IID layout_sink_iid={0x2af2d06a,0xdd5b,0x4927,{0xa0,0xb4,0x54,0xf1,0x9c,0x91,0xfa,0xde}};
}
#endif
