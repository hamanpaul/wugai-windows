#ifndef BOSHIAMY_ROOT_CHART_WINDOW_HPP
#define BOSHIAMY_ROOT_CHART_WINDOW_HPP
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "root_chart.hpp"
#include "root_chart_qglyph_render.hpp"
#include "root_chart_rs_glyph_render.hpp"
#include "root_chart_shape_glyph_render.hpp"
#include "root_chart_f_meaning_glyph_render.hpp"
#include "root_chart_k_meaning_glyph_render.hpp"
#include "root_chart_t_meaning_glyph_render.hpp"
#include "root_chart_f_sound_glyph_render.hpp"
#include "root_chart_eg_sound_glyph_render.hpp"
namespace boshiamy {
struct RootChartWindowGlyphTestAccess;
class RootChartWindow {
public:
 explicit RootChartWindow(HINSTANCE instance):instance_(instance){}
 RootChartWindow(const RootChartWindow&)=delete;
 RootChartWindow& operator=(const RootChartWindow&)=delete;
 ~RootChartWindow(){if(hwnd_&&IsWindow(hwnd_))DestroyWindow(hwnd_);if(owns_class_)UnregisterClassW(class_name(),instance_);}
 bool create(HWND owner=nullptr){if(hwnd_&&IsWindow(hwnd_))return true;WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=wndproc;wc.hInstance=instance_;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName=class_name();if(RegisterClassExW(&wc))owns_class_=true;else if(GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;HMONITOR monitor=owner?MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST):MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};RECT work{0,0,1320,820};if(monitor&&GetMonitorInfoW(monitor,&mi))work=mi.rcWork;int work_width=std::max(100,static_cast<int>(work.right-work.left));int work_height=std::max(100,static_cast<int>(work.bottom-work.top));int width=std::min(1320,std::max(100,work_width-16));int height=std::min(820,std::max(100,work_height-16));int x=work.left+std::max(0,(work_width-width)/2);int y=work.top+std::max(0,(work_height-height)/2);hwnd_=CreateWindowExW(WS_EX_TOOLWINDOW,class_name(),L"嘸蝦米字根表",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN|WS_VSCROLL|WS_HSCROLL,x,y,width,height,owner,nullptr,instance_,this);return hwnd_!=nullptr;}
 void show(int command=SW_SHOWNORMAL){if(hwnd_){ShowWindow(hwnd_,command);UpdateWindow(hwnd_);}}
 HWND hwnd()const{return hwnd_;}
private:
 friend struct RootChartWindowGlyphTestAccess;
 HINSTANCE instance_;HWND hwnd_=nullptr;bool owns_class_=false;int scroll_x_=0,scroll_y_=0;static constexpr int header_height_=38,left_=12;
 static const wchar_t* class_name(){return L"BoshiamyRootChartWindowClass";}
 static RootChartWindow* self(HWND hwnd){return reinterpret_cast<RootChartWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));}
 static constexpr int widths_[6]={54,205,320,425,62,90};
 static constexpr COLORREF row_white_=RGB(255,255,255),row_yellow_=RGB(255,253,227),shape_green_=RGB(220,238,216),sound_pink_=RGB(248,220,228),meaning_blue_=RGB(211,233,247),grid_=RGB(50,45,42);
 static int content_height(){int total=header_height_;for(const auto& row:root_chart_rows)total+=row.height;return total;}
 static int content_width(){int total=left_+90;for(int w:widths_)total+=w;return total;}
 static LRESULT CALLBACK wndproc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){RootChartWindow* p=self(hwnd);if(message==WM_NCCREATE){p=static_cast<RootChartWindow*>(((CREATESTRUCTW*)lparam)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));p->hwnd_=hwnd;}if(!p)return DefWindowProcW(hwnd,message,wparam,lparam);if(message==WM_SIZE){p->update_scrollbars();return 0;}if(message==WM_VSCROLL){p->scroll(wparam);return 0;}if(message==WM_HSCROLL){p->scroll_horizontal(wparam);return 0;}if(message==WM_MOUSEWHEEL){int lines=std::max(1,(int)(wparam>>16)&0xff);p->scroll_by(-(GET_WHEEL_DELTA_WPARAM(wparam)/WHEEL_DELTA)*lines*32);return 0;}if(message==WM_ERASEBKGND)return 1;if(message==WM_PAINT){p->paint();return 0;}if(message==WM_DESTROY)return 0;if(message==WM_NCDESTROY){p->hwnd_=nullptr;SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);}return DefWindowProcW(hwnd,message,wparam,lparam);}
 void update_scrollbars(){if(!hwnd_)return;RECT r{};GetClientRect(hwnd_,&r);SCROLLINFO vertical{sizeof(vertical),SIF_RANGE|SIF_PAGE|SIF_POS};vertical.nMin=0;vertical.nMax=std::max(0,content_height()-1);vertical.nPage=static_cast<UINT>(std::max(0,static_cast<int>(r.bottom)-header_height_));vertical.nPos=scroll_y_;SetScrollInfo(hwnd_,SB_VERT,&vertical,TRUE);scroll_y_=std::min(scroll_y_,std::max(0,content_height()-static_cast<int>(vertical.nPage)));SetScrollPos(hwnd_,SB_VERT,scroll_y_,TRUE);SCROLLINFO horizontal{sizeof(horizontal),SIF_RANGE|SIF_PAGE|SIF_POS};horizontal.nMin=0;horizontal.nMax=std::max(0,content_width()-1);horizontal.nPage=static_cast<UINT>(std::max(0,static_cast<int>(r.right)));horizontal.nPos=scroll_x_;SetScrollInfo(hwnd_,SB_HORZ,&horizontal,TRUE);scroll_x_=std::min(scroll_x_,std::max(0,content_width()-static_cast<int>(horizontal.nPage)));SetScrollPos(hwnd_,SB_HORZ,scroll_x_,TRUE);ShowScrollBar(hwnd_,SB_HORZ,content_width()>static_cast<int>(r.right));}
 void scroll_by(int delta){RECT r{};GetClientRect(hwnd_,&r);int page=std::max(0,static_cast<int>(r.bottom)-header_height_);int next=std::clamp(scroll_y_+delta,0,std::max(0,content_height()-page));if(next==scroll_y_)return;scroll_y_=next;SetScrollPos(hwnd_,SB_VERT,scroll_y_,TRUE);InvalidateRect(hwnd_,nullptr,TRUE);}
 void scroll_horizontal_by(int delta){RECT r{};GetClientRect(hwnd_,&r);int next=std::clamp(scroll_x_+delta,0,std::max(0,content_width()-static_cast<int>(r.right)));if(next==scroll_x_)return;scroll_x_=next;SetScrollPos(hwnd_,SB_HORZ,scroll_x_,TRUE);InvalidateRect(hwnd_,nullptr,TRUE);}
 void scroll(WPARAM command){SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(hwnd_,SB_VERT,&info);int next=scroll_y_;switch(LOWORD(command)){case SB_LINEUP:next-=32;break;case SB_LINEDOWN:next+=32;break;case SB_PAGEUP:next-=static_cast<int>(info.nPage);break;case SB_PAGEDOWN:next+=static_cast<int>(info.nPage);break;case SB_THUMBTRACK:case SB_THUMBPOSITION:next=info.nTrackPos;break;case SB_TOP:next=0;break;case SB_BOTTOM:next=content_height();break;default:return;}scroll_by(next-scroll_y_);}
 void scroll_horizontal(WPARAM command){SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(hwnd_,SB_HORZ,&info);int next=scroll_x_;switch(LOWORD(command)){case SB_LINELEFT:next-=32;break;case SB_LINERIGHT:next+=32;break;case SB_PAGELEFT:next-=static_cast<int>(info.nPage);break;case SB_PAGERIGHT:next+=static_cast<int>(info.nPage);break;case SB_THUMBTRACK:case SB_THUMBPOSITION:next=info.nTrackPos;break;case SB_LEFT:next=0;break;case SB_RIGHT:next=content_width();break;default:return;}scroll_horizontal_by(next-scroll_x_);}
 static void text(HDC dc,const std::wstring_view value,RECT box,UINT flags=DT_LEFT|DT_VCENTER|DT_WORDBREAK){if(value.empty())return;std::wstring copy(value);DrawTextW(dc,copy.c_str(),static_cast<int>(copy.size()),&box,flags|DT_NOPREFIX);}
 static bool is_chart_glyph(wchar_t c){return c>=root_chart_glyph_e0&&c<=root_chart_glyph_gs3;}
 static bool contains_chart_glyph(std::wstring_view value){for(wchar_t c:value)if(is_chart_glyph(c))return true;return false;}
 // Normalized strokes traced from the corresponding cells in bitmap 113.
 // These are custom components, not claims that a similar Unicode glyph exists.
 static void draw_chart_glyph(HDC dc,wchar_t token,RECT box){
  if(draw_root_chart_q_glyph(dc,token,box))return;
  if(token==root_chart_glyph_q6){draw_root_chart_q7_glyph(dc,box);return;}
  if(draw_root_chart_rs_glyph(dc,token,box))return;
  if(draw_root_chart_shape_glyph(dc,token,box))return;
  if(draw_root_chart_f_meaning_glyph(dc,token,box))return;
  if(draw_root_chart_k_meaning_glyph(dc,token,box))return;
  if(draw_root_chart_t_meaning_glyph(dc,token,box))return;
  if(draw_root_chart_f_sound_glyph(dc,token,box))return;
  if(draw_root_chart_eg_sound_glyph(dc,token,box))return;
  using P=POINT;const P* paths[6]{};int counts[6]{};
  P b0a[]={{230,80},{230,920}}; P b0b[]={{230,100},{650,100},{850,220},{850,380},{680,480},{230,480}}; P b0c[]={{230,500},{680,500},{850,640},{850,790},{650,900},{230,900}};
  P e0a[]={{120,650},{330,330},{520,470},{760,330}}; P e0b[]={{250,710},{500,820},{720,710}};
  P e1a[]={{420,900},{420,130},{820,80}},e1b[]={{420,380},{850,340}},e1c[]={{420,660},{800,630}};
  P e2a[]={{80,250},{800,160},{840,300},{190,350},{180,560},{760,530}};
  P x0a[]={{130,180},{840,180},{700,390},{250,850}}; P x0b[]={{160,750},{500,470},{850,220}};
  P x1a[]={{250,850},{360,600},{520,400},{680,210},{820,120}}; P x1b[]={{180,300},{430,420},{700,520}};
  P x2a[]={{500,100},{500,900}}; P x2b[]={{200,420},{500,420},{820,600}};
  P y0a[]={{320,90},{320,520},{280,680},{190,800},{90,850},{120,900}};
  P z0a[]={{180,220},{760,180},{500,430},{270,620},{300,800},{520,900},{780,820}};
  P z1a[]={{160,780},{330,540},{520,380},{720,250},{860,120}};
  P f0a[]={{120,520},{360,430},{470,480},{610,380},{820,300}}; P f0b[]={{250,570},{410,660},{560,560}};
  P f1a[]={{190,260},{780,210}}; P f1b[]={{340,180},{340,640},{250,840},{160,790}};
  P f2a[]={{300,250},{740,250}}; P f2b[]={{360,400},{700,400}}; P f2c[]={{520,220},{520,840}};
  P f3a[]={{480,120},{480,850}}; P f3b[]={{420,420},{560,420}};
  P f4a[]={{500,180},{740,270},{800,500},{700,760},{500,850},{300,750},{230,500},{300,280},{500,180}};
  P f5a[]={{780,120},{220,880}};
  P f6a[]={{500,180},{220,800}}; P f6b[]={{500,180},{820,800}};
  P r0a[]={{300,160},{270,720},{190,850},{300,900},{430,850}};
  P r1a[]={{360,160},{360,850}}; P r1b[]={{660,160},{660,850}};
  P r2a[]={{250,180},{760,180},{760,800},{250,800},{250,180}};
  P r3a[]={{500,140},{500,860}}; P r3b[]={{160,500},{830,500}};
  P r4a[]={{500,120},{500,850}}; P r4b[]={{170,330},{820,330}}; P r4c[]={{260,720},{740,720}};
  P s0a[]={{250,160},{220,810},{330,900}}; P s0b[]={{690,160},{720,810},{610,900}};
  P s1a[]={{300,180},{300,400},{270,570}}; P s1b[]={{700,180},{700,400},{730,570}}; P s1c[]={{180,650},{500,650},{820,650}};
  P s2a[]={{240,180},{760,180}}; P s2b[]={{500,180},{360,480},{210,810}}; P s2c[]={{500,180},{650,480},{820,810}};
  P s3a[]={{160,220},{830,220}}; P s3b[]={{500,120},{500,850}}; P s3c[]={{190,650},{810,650}};
  P s4a[]={{300,160},{300,700},{220,860}}; P s4b[]={{650,160},{650,700},{780,860}}; P s4c[]={{300,480},{650,480}};
  P s5a[]={{220,180},{780,180}}; P s5b[]={{300,180},{300,830}}; P s5c[]={{650,180},{650,500},{820,500},{820,830}}; P s5d[]={{300,510},{650,510}};
  switch(token){case root_chart_glyph_b0:paths[0]=b0a;counts[0]=2;paths[1]=b0b;counts[1]=6;paths[2]=b0c;counts[2]=6;break;
   case root_chart_glyph_e0:paths[0]=e0a;counts[0]=4;paths[1]=e0b;counts[1]=3;break;
   case root_chart_glyph_e1:paths[0]=e1a;counts[0]=3;paths[1]=e1b;counts[1]=2;paths[2]=e1c;counts[2]=2;break;
   case root_chart_glyph_e2:paths[0]=e2a;counts[0]=6;break;
   case root_chart_glyph_x0:paths[0]=x0a;counts[0]=4;paths[1]=x0b;counts[1]=3;break;
   case root_chart_glyph_x1:paths[0]=x1a;counts[0]=5;paths[1]=x1b;counts[1]=3;break;
   case root_chart_glyph_x2:paths[0]=x2a;counts[0]=2;paths[1]=x2b;counts[1]=3;break;
   case root_chart_glyph_y0:paths[0]=y0a;counts[0]=6;break;
   case root_chart_glyph_z0:paths[0]=z0a;counts[0]=7;break;
   case root_chart_glyph_z1:paths[0]=z1a;counts[0]=5;break;
   case root_chart_glyph_f0:paths[0]=f0a;counts[0]=5;paths[1]=f0b;counts[1]=3;break;
   case root_chart_glyph_f1:paths[0]=f1a;counts[0]=2;paths[1]=f1b;counts[1]=4;break;
   case root_chart_glyph_f2:paths[0]=f2a;counts[0]=2;paths[1]=f2b;counts[1]=2;paths[2]=f2c;counts[2]=2;break;
   case root_chart_glyph_f3:paths[0]=f3a;counts[0]=2;paths[1]=f3b;counts[1]=2;break;
   case root_chart_glyph_f4:paths[0]=f4a;counts[0]=9;break;
   case root_chart_glyph_f5:paths[0]=f5a;counts[0]=2;break;
   case root_chart_glyph_f6:paths[0]=f6a;counts[0]=2;paths[1]=f6b;counts[1]=2;break;
   case root_chart_glyph_r0:paths[0]=r0a;counts[0]=5;break;
   case root_chart_glyph_r1:paths[0]=r1a;counts[0]=2;paths[1]=r1b;counts[1]=2;break;
   case root_chart_glyph_r2:paths[0]=r2a;counts[0]=5;break;
   case root_chart_glyph_r3:paths[0]=r3a;counts[0]=2;paths[1]=r3b;counts[1]=2;break;
   case root_chart_glyph_r4:paths[0]=r4a;counts[0]=2;paths[1]=r4b;counts[1]=2;paths[2]=r4c;counts[2]=2;break;
   case root_chart_glyph_s0:paths[0]=s0a;counts[0]=3;paths[1]=s0b;counts[1]=3;break;
   case root_chart_glyph_s1:paths[0]=s1a;counts[0]=3;paths[1]=s1b;counts[1]=3;paths[2]=s1c;counts[2]=3;break;
   case root_chart_glyph_s2:paths[0]=s2a;counts[0]=2;paths[1]=s2b;counts[1]=3;paths[2]=s2c;counts[2]=3;break;
   case root_chart_glyph_s3:paths[0]=s3a;counts[0]=2;paths[1]=s3b;counts[1]=2;paths[2]=s3c;counts[2]=2;break;
   case root_chart_glyph_s4:paths[0]=s4a;counts[0]=3;paths[1]=s4b;counts[1]=3;paths[2]=s4c;counts[2]=2;break;
   case root_chart_glyph_s5:paths[0]=s5a;counts[0]=2;paths[1]=s5b;counts[1]=3;paths[2]=s5c;counts[2]=4;paths[3]=s5d;counts[3]=2;break;default:return;}
  HPEN pen=CreatePen(PS_SOLID,2,RGB(35,32,30));HGDIOBJ old=SelectObject(dc,pen);int w=std::max(1,static_cast<int>(box.right-box.left));int h=std::max(1,static_cast<int>(box.bottom-box.top));
  for(int i=0;i<6;++i)if(paths[i]){POINT scaled[16]{};for(int j=0;j<counts[i];++j){scaled[j].x=box.left+paths[i][j].x*w/1000;scaled[j].y=box.top+paths[i][j].y*h/1000;}Polyline(dc,scaled,counts[i]);}
  SelectObject(dc,old);DeleteObject(pen);
 }
 template<std::size_t N,class Glyph> static bool source_glyph_size_in(const std::array<Glyph,N>& glyphs,wchar_t token,int& width,int& height){for(const auto& glyph:glyphs)if(glyph.token==token){width=glyph.width;height=glyph.height;return true;}return false;}
 static bool source_glyph_size(wchar_t token,int& width,int& height){
  if(token==root_chart_glyph_q6){width=root_chart_q7_source_width;height=root_chart_q7_source_height;return true;}
  return source_glyph_size_in(root_chart_q_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_rs_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_shape_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_f_meaning_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_k_meaning_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_t_meaning_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_f_sound_ink_glyphs,token,width,height)||
      source_glyph_size_in(root_chart_eg_sound_ink_glyphs,token,width,height);
 }
 static int source_height_on_line(std::wstring_view value,size_t begin,size_t end){int max_height=0;for(size_t i=begin;i<end;++i){int glyph_width=0,glyph_height=0;if(is_chart_glyph(value[i])&&source_glyph_size(value[i],glyph_width,glyph_height))max_height=std::max(max_height,glyph_height);}return max_height;}
 static void shape_text(HDC dc,std::wstring_view value,RECT box){
  if(value.empty())return;
  int line_count=1;for(wchar_t c:value)if(c==L'\n')++line_count;
  const int box_height=static_cast<int>(box.bottom-box.top);
  const int line_height=std::max(18,box_height/line_count);
  int em_height=18;TEXTMETRICW metrics{};if(GetTextMetricsW(dc,&metrics)){const int measured=static_cast<int>(metrics.tmHeight-metrics.tmInternalLeading);if(measured>0)em_height=measured;}
  const double nominal_scale=static_cast<double>(em_height)/11.0;
  int x=box.left,y=box.top+(box_height-line_count*line_height)/2;
  size_t line_begin=0;
  for(size_t i=0;i<=value.size();++i){
   if(i<value.size()&&value[i]!=L'\n')continue;
   const int max_source_height=source_height_on_line(value,line_begin,i);
   double scale=nominal_scale;
   if(max_source_height>0){const int available_height=std::max(1,line_height-5);scale=std::min(scale,static_cast<double>(available_height)/max_source_height);}
   for(size_t j=line_begin;j<i;++j){const wchar_t c=value[j];
    if(c==L' '){x+=7;continue;}
    if(is_chart_glyph(c)){
     int source_width=0,source_height=0;
     if(source_glyph_size(c,source_width,source_height)){
      const int glyph_width=std::max(1,static_cast<int>(std::ceil(source_width*scale)));
      const int glyph_height=std::max(1,static_cast<int>(std::ceil(source_height*scale)));
      const int top=y+(line_height-glyph_height)/2;
      RECT glyph_box{x+1,top,x+1+glyph_width,top+glyph_height};
      draw_chart_glyph(dc,c,glyph_box);x+=glyph_width+2;
     }else{
      RECT glyph_box{x+1,y+2,x+19,y+line_height-3};draw_chart_glyph(dc,c,glyph_box);x+=22;
     }
     continue;
    }
    SIZE size{};GetTextExtentPoint32W(dc,&c,1,&size);TextOutW(dc,x,y+std::max(0,(line_height-static_cast<int>(size.cy))/2),&c,1);x+=std::max(8,static_cast<int>(size.cx));
   }
   if(i<value.size()){x=box.left;y+=line_height;line_begin=i+1;}
  }
 }
 void paint(){PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd_,&ps);RECT client{};GetClientRect(hwnd_,&client);FillRect(dc,&client,(HBRUSH)GetStockObject(WHITE_BRUSH));SetBkMode(dc,TRANSPARENT);HFONT font=CreateFontW(18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft JhengHei UI");HGDIOBJ old=font?SelectObject(dc,font):nullptr;int x=left_-scroll_x_;const COLORREF colors[6]={RGB(255,255,255),shape_green_,sound_pink_,meaning_blue_,RGB(255,255,255),RGB(255,255,255)};const std::wstring_view heads[6]={L"",root_chart_shape_header,root_chart_sound_header,root_chart_meaning_header,L"",L""};int headerY=0;for(int col=0;col<6;++col){RECT cell{x,headerY,x+widths_[col],header_height_};HBRUSH brush=CreateSolidBrush(colors[col]);FillRect(dc,&cell,brush);DeleteObject(brush);text(dc,heads[col],cell,DT_CENTER|DT_VCENTER|DT_SINGLELINE);FrameRect(dc,&cell,(HBRUSH)GetStockObject(BLACK_BRUSH));x+=widths_[col];}int y=header_height_-scroll_y_;for(size_t i=0;i<root_chart_rows.size();++i){const auto& row=root_chart_rows[i];int h=row.height;if(y+h>=header_height_&&y<client.bottom){const COLORREF background=(i%2==1)?row_yellow_:row_white_;x=left_-scroll_x_;const std::wstring values[6]={std::wstring(1,(wchar_t)row.key),std::wstring(row.shape),std::wstring(row.sound),std::wstring(row.meaning),std::wstring(row.supplement),L""};for(int col=0;col<6;++col){RECT cell{x,y,x+widths_[col],y+h};HBRUSH brush=CreateSolidBrush(background);FillRect(dc,&cell,brush);DeleteObject(brush);if(col<5){RECT inner=cell;InflateRect(&inner,-5,-2);if(col==1||contains_chart_glyph(values[col]))shape_text(dc,values[col],inner);else text(dc,values[col],inner,col==0?DT_CENTER|DT_VCENTER|DT_SINGLELINE:DT_LEFT|DT_VCENTER|DT_WORDBREAK);}FrameRect(dc,&cell,(HBRUSH)GetStockObject(BLACK_BRUSH));x+=widths_[col];}draw_margin_notes(dc,static_cast<char>(row.key),y,h,left_+widths_[0]+widths_[1]+widths_[2]+widths_[3]+widths_[4]+widths_[5]-scroll_x_);}y+=h;}if(old)SelectObject(dc,old);if(font)DeleteObject(font);EndPaint(hwnd_,&ps);}
 static void draw_margin_notes(HDC dc,char key,int y,int row_height,int x){for(const auto& note:root_chart_margin_notes){int first=note.first_key-'A',last=note.last_key-'A',current=key-'A';if(current<first||current>last)continue;std::wstring value;size_t line=0,start=0;for(size_t i=0;i<note.text.size();++i)if(note.text[i]==L'\n'){if(current-first==static_cast<int>(line)){value=std::wstring(note.text.substr(start,i-start));break;}++line;start=i+1;}if(value.empty()&&current-first==static_cast<int>(line))value=std::wstring(note.text.substr(start));RECT r{x,y,x+90,y+row_height};text(dc,value,r,DT_CENTER|DT_VCENTER|DT_WORDBREAK);}}
};
}
#endif
