#ifndef BOSHIAMY_ROOT_CHART_Q7_RENDER_HPP
#define BOSHIAMY_ROOT_CHART_Q7_RENDER_HPP
#include <algorithm>
#include <cstddef>
#include <windows.h>
#include "root_chart_q7_glyph.hpp"
namespace boshiamy {
template<class Run>
inline void draw_root_chart_source_ink_glyph(HDC dc,RECT box,int source_width,int source_height,const Run* runs,std::size_t run_count){
 const int box_width=static_cast<int>(box.right-box.left),box_height=static_cast<int>(box.bottom-box.top);
 if(!dc||box_width<=0||box_height<=0||source_width<=0||source_height<=0||!runs)return;
 int dest_width=0,dest_height=0;
 if(box_width*source_height<=box_height*source_width){
  dest_width=box_width;
  dest_height=dest_width*source_height/source_width;
 }else{
  dest_width=box_height*source_width/source_height;
  dest_height=dest_width*source_height/source_width;
 }
 dest_width=std::max(1,std::min(box_width,dest_width));
 dest_height=std::max(1,std::min(box_height,dest_height));
 const int left=box.left+(box_width-dest_width)/2;
 const int top=box.top+(box_height-dest_height)/2;
 for(std::size_t i=0;i<run_count;++i){const auto& run=runs[i];
  const int x0=(static_cast<int>(run.x0)*dest_width+source_width-1)/source_width;
  const int x1=(static_cast<int>(run.x1)*dest_width+source_width-1)/source_width;
  const int y0=(static_cast<int>(run.y)*dest_height+source_height-1)/source_height;
  const int y1=(static_cast<int>(run.y+1)*dest_height+source_height-1)/source_height;
  if(x0>=x1||y0>=y1)continue;
  RECT pixel{left+x0,top+y0,left+x1,top+y1};
  HBRUSH brush=CreateSolidBrush(RGB(run.r,run.g,run.b));
  if(brush){FillRect(dc,&pixel,brush);DeleteObject(brush);}
 }
}
inline void draw_root_chart_q7_glyph(HDC dc,RECT box){
 draw_root_chart_source_ink_glyph(dc,box,root_chart_q7_source_width,root_chart_q7_source_height,root_chart_q7_ink_runs.data(),root_chart_q7_ink_runs.size());
}
}
#endif
