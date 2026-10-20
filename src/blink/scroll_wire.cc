#include "blink/scroll_wire.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include "third_party/zlib/zlib.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"
#include "third_party/skia/include/core/SkPicture.h"
#include "third_party/skia/include/core/SkSerialProcs.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkTypeface.h"
#if defined(UEFI_RUNTIME)
extern "C" size_t runtime_memory_used(void);
#endif
namespace uefi_browser {
bool ScrollWireMemoryAvailable(size_t reserve) {
#if defined(UEFI_RUNTIME)
  constexpr size_t limit=128*1024*1024;
  size_t used=runtime_memory_used();
  return reserve<=limit && used<=limit-reserve;
#else
  return true;
#endif
}
namespace {
static_assert(sizeof(unsigned)==4 && sizeof(int)==4 && sizeof(float)==4);
constexpr uint32_t kMagic=0x5343524c, kVersion=2;
// This limits total decoding work; only one tile is expanded at a time.
constexpr size_t kPixelBudget=256*1024*1024;
constexpr float kCoordinateLimit=1000000;
struct Writer {
  static constexpr bool reading=false;
  bool valid=true;
  std::vector<uint8_t> bytes;
  bool Reserve(size_t count) {
    if (!valid || count>kScrollWireCapacity-bytes.size()) { valid=false; return false; }
    if (bytes.size()+count>bytes.capacity()) {
      size_t capacity=std::min(kScrollWireCapacity,std::max(bytes.size()+count,std::max(size_t(256),bytes.capacity()*2)));
      if (!ScrollWireMemoryAvailable(capacity+4*1024*1024)) { std::puts("COMPOSITOR: wire buffer headroom exhausted"); valid=false; return false; }
      bytes.reserve(capacity);
    }
    return true;
  }
  void Word(uint32_t& value) {
    if (!Reserve(4)) return;
    for (unsigned shift=0;shift<32;shift+=8) bytes.push_back(value>>shift);
  }
};
struct Reader {
  static constexpr bool reading=true;
  bool valid=true;
  const uint8_t* bytes;
  size_t size, position=0, records=32768;
  void Word(uint32_t& value) {
    if (!valid || size-position<4) { valid=false; value=0; return; }
    value=0;
    for (unsigned shift=0;shift<32;shift+=8) value|=uint32_t(bytes[position++])<<shift;
  }
};
template<class Archive> void Number(Archive& archive,unsigned& value) { archive.Word(value); }
template<class Archive> void Number(Archive& archive,int& value) {
  uint32_t bits; std::memcpy(&bits,&value,4); archive.Word(bits); std::memcpy(&value,&bits,4);
  if (value < -1000000 || value > 1000000) archive.valid=false;
}
template<class Archive> void Number(Archive& archive,uint64_t& value) {
  uint32_t low=static_cast<uint32_t>(value), high=static_cast<uint32_t>(value>>32); archive.Word(low); archive.Word(high); value=uint64_t(high)<<32|low;
}
template<class Archive> void Number(Archive& archive,bool& value) {
  uint32_t bits=value; archive.Word(bits); if (bits>1) archive.valid=false; value=bits==1;
}
template<class Archive> void Number(Archive& archive,float& value) {
  uint32_t bits; std::memcpy(&bits,&value,4); archive.Word(bits); std::memcpy(&value,&bits,4);
  if (!std::isfinite(value) || std::abs(value)>kCoordinateLimit) archive.valid=false;
}
template<class Archive> void Number(Archive& archive,cc::ElementId& value) {
  uint64_t identifier=value.GetInternalValue(); Number(archive,identifier); value=identifier ? cc::ElementId(identifier) : cc::ElementId();
}
template<class Archive,class... Values> void Fields(Archive& archive,Values&... values) { (Number(archive,values),...); }
template<class Archive> void Rectangle(Archive& archive,SkRect& rectangle) {
  Fields(archive,rectangle.fLeft,rectangle.fTop,rectangle.fRight,rectangle.fBottom);
  if (!rectangle.isSorted()) archive.valid=false;
}
template<class Archive> void Rectangle(Archive& archive,gfx::RectF& rectangle) {
  float x=rectangle.x(),y=rectangle.y(),width=rectangle.width(),height=rectangle.height();
  Fields(archive,x,y,width,height); if (width<0 || height<0) archive.valid=false;
  rectangle=gfx::RectF(x,y,width,height);
}
template<class Archive> void Item(Archive& archive,SkRect& value) { Rectangle(archive,value); }
template<class Archive,class ItemType> void Items(Archive& archive,std::vector<ItemType>& values,unsigned limit=4096) {
  if (values.size()>limit) { archive.valid=false; return; }
  unsigned count=static_cast<unsigned>(values.size()); Number(archive,count);
  if (!archive.valid || count>limit) { archive.valid=false; return; }
  if constexpr (Archive::reading) {
    if (count>archive.records) { archive.valid=false; return; }
    archive.records-=count; values.resize(count);
  }
  for (auto& value:values) { if (!archive.valid) break; Item(archive,value); }
}
template<class Archive> void Item(Archive& archive,ScrollTransform& value) {
  Fields(archive,value.parent,value.x,value.y,value.element,value.scroll_parent,value.sticky_element,
      value.position,value.minimum,value.maximum,value.viewport,value.position_x,value.minimum_x,
      value.maximum_x,value.viewport_x,value.horizontal,value.vertical,value.chain_x,value.chain,value.sticky);
  auto& constraint=value.constraint;
  Fields(archive,constraint.is_anchored_left,constraint.is_anchored_right,constraint.is_anchored_top,
      constraint.is_anchored_bottom,constraint.left_offset,constraint.right_offset,constraint.top_offset,constraint.bottom_offset);
  Rectangle(archive,constraint.constraint_box_rect);
  Rectangle(archive,constraint.scroll_container_relative_sticky_box_rect);
  Rectangle(archive,constraint.scroll_container_relative_containing_block_rect);
  float x=constraint.pixel_snap_offset.x(), y=constraint.pixel_snap_offset.y(); Fields(archive,x,y);
  constraint.pixel_snap_offset=gfx::Vector2dF(x,y);
  Fields(archive,constraint.x_scroll_ancestor_element_id,constraint.y_scroll_ancestor_element_id,
      constraint.nearest_element_shifting_sticky_box,constraint.nearest_element_shifting_containing_block);
}
template<class Archive> void Item(Archive& archive,ScrollClip& value) {
  Fields(archive,value.parent,value.transform);
  auto rectangle=value.rectangle.rect(); Rectangle(archive,rectangle);
  SkVector radii[4];
  for (unsigned index=0;index<4;++index) {
    radii[index]=value.rectangle.radii(static_cast<SkRRect::Corner>(index));
    Fields(archive,radii[index].fX,radii[index].fY);
    if (radii[index].fX<0 || radii[index].fY<0) archive.valid=false;
  }
  value.rectangle.setRectRadii(rectangle,radii);
  if (!value.rectangle.isValid()) archive.valid=false;
}
template<class Archive> void Item(Archive& archive,ScrollEffect& value) {
  Fields(archive,value.parent,value.clip,value.opacity);
  unsigned blend=static_cast<unsigned>(value.blend); Number(archive,blend);
  if (blend>static_cast<unsigned>(SkBlendMode::kLastMode) || value.opacity<0 || value.opacity>1) archive.valid=false;
  value.blend=static_cast<SkBlendMode>(blend);
}
template<class Archive> void Item(Archive& archive,ScrollRegion& value) { Fields(archive,value.transform,value.clip); Rectangle(archive,value.bounds); }
template<class Archive> void Item(Archive& archive,ScrollGutter& value) {
  Fields(archive,value.element,value.horizontal,value.thickness);
  for (auto& size:value.states) Number(archive,size);
}
template<class Archive> void Item(Archive& archive,ScrollQuery& value) { Fields(archive,value.element,value.horizontal,value.vertical,value.stuck); }
template<class Archive> void Item(Archive& archive,ScrollbarLayout& value) {
  Fields(archive,value.hover,value.press);
  Rectangle(archive,value.track); Rectangle(archive,value.thumb);
  Rectangle(archive,value.back_start); Rectangle(archive,value.back_end);
  Rectangle(archive,value.forward_start); Rectangle(archive,value.forward_end);
}
template<class Archive> void Item(Archive& archive,ScrollbarScene& value) {
  Fields(archive,value.element,value.transform,value.clip,value.thumb_transform,value.track_picture,
      value.thumb_picture,value.last_picture,value.horizontal,value.jump,value.custom,value.enabled);
  for (auto& size:value.thickness) Number(archive,size);
  Fields(archive,value.painted_hover,value.painted_press,value.hover,value.layout,value.initial_delay,value.repeat_delay);
  Rectangle(archive,value.back_start); Rectangle(archive,value.back_end);
  Rectangle(archive,value.forward_start); Rectangle(archive,value.forward_end);
  Rectangle(archive,value.frame); Rectangle(archive,value.track); Rectangle(archive,value.thumb);
  Items(archive,value.layouts,128);
}
template<class Archive> void Item(Archive& archive,ScrollPicture& value) {
  Fields(archive,value.transform,value.clip,value.effect,value.scrollbar,value.track_piece,value.part);
  Rectangle(archive,value.bounds); Items(archive,value.appearance_bounds,128);
}
template<class Archive> void Metadata(Archive& archive,ScrollScene& scene) {
  Fields(archive,scene.wheel_listeners,scene.wheel_blocked);
  Items(archive,scene.transforms); Items(archive,scene.clips); Items(archive,scene.effects,128);
  Items(archive,scene.wheel_regions); Items(archive,scene.scrollbars,128);
  Items(archive,scene.gutters,128); Items(archive,scene.queries,128); Items(archive,scene.pictures,2048);
}
bool Index(int index,size_t count) { return index>=-1 && (index<0 || size_t(index)<count); }
template<class Node> bool Tree(const std::vector<Node>& nodes) {
  for (size_t index=0;index<nodes.size();++index) {
    if (!Index(nodes[index].parent,index)) return false;
    unsigned depth=0;
    for (int parent=nodes[index].parent;parent>=0;parent=nodes[parent].parent) if (++depth>64) return false;
  }
  return true;
}
bool Validate(const ScrollScene& scene,bool diagnostic=false) {
  auto reject=[&](int line) { if (diagnostic) std::printf("COMPOSITOR: wire invalid graph line=%d\n",line); return false; };
  if (!Tree(scene.transforms) || !Tree(scene.clips) || !Tree(scene.effects)) return reject(__LINE__);
  for (const auto& node:scene.transforms) {
    if (node.minimum>node.position || node.position>node.maximum || node.minimum_x>node.position_x ||
        node.position_x>node.maximum_x || node.viewport<0 || node.viewport_x<0) return reject(__LINE__);
  }
  for (const auto& node:scene.clips) if (!Index(node.transform,scene.transforms.size())) return reject(__LINE__);
  for (const auto& node:scene.effects) if (!Index(node.clip,scene.clips.size())) return reject(__LINE__);
  for (const auto& node:scene.wheel_regions)
    if (!Index(node.transform,scene.transforms.size()) || !Index(node.clip,scene.clips.size())) return reject(__LINE__);
  for (const auto& bar:scene.scrollbars) {
    if (!Index(bar.transform,scene.transforms.size()) || !Index(bar.clip,scene.clips.size()) ||
        bar.thumb_transform<0 || !Index(bar.thumb_transform,scene.transforms.size()) ||
        bar.track_picture>bar.last_picture || bar.last_picture>=scene.pictures.size() ||
        // Disabled native scrollbars can omit the thumb and leave its picture index at zero.
        bar.thumb_picture>=scene.pictures.size() ||
        bar.initial_delay<1 || bar.repeat_delay<1 || bar.initial_delay>60000 || bar.repeat_delay>60000 ||
        (bar.layouts.empty() ? bar.layout!=0 : bar.layout>=bar.layouts.size())) return reject(__LINE__);
  }
  for (const auto& picture:scene.pictures) {
    if (!Index(picture.transform,scene.transforms.size()) || !Index(picture.clip,scene.clips.size()) ||
        !Index(picture.effect,scene.effects.size()) || !Index(picture.scrollbar,scene.scrollbars.size()) ||
        picture.track_piece < -1 || picture.track_piece > 1) return reject(__LINE__);
    size_t expected=1;
    if (picture.scrollbar>=0) {
      const auto& bar=scene.scrollbars[picture.scrollbar]; expected=bar.layouts.empty() ? 8 : bar.layouts.size();
      if (!picture.appearance_bounds.empty() && picture.appearance_bounds.size()!=bar.layouts.size()) return reject(__LINE__);
    } else if (!picture.appearance_bounds.empty()) return reject(__LINE__);
    if (!picture.raster_appearances.empty() && picture.raster_appearances.size()!=expected) return reject(__LINE__);
  }
  for (const auto& bar:scene.scrollbars) for (int thickness:bar.thickness) if (thickness<0 || thickness>4096) return reject(__LINE__);
  for (const auto& gutter:scene.gutters) {
    if (gutter.thickness<0 || gutter.thickness>4096) return reject(__LINE__);
    for (int thickness:gutter.states) if (thickness<0 || thickness>4096) return reject(__LINE__);
  }
  for (const auto& query:scene.queries) if (query.horizontal>4 || query.vertical>4) return reject(__LINE__);
  return true;
}
struct Resources {
  std::vector<sk_sp<SkImage>> images;
  std::vector<sk_sp<SkTypeface>> fonts;
  bool Load(const ScrollScene& scene) {
    if (scene.renderer_images.size()!=scene.images.size() || scene.renderer_fonts.size()!=scene.fonts.size()) return false;
    images=scene.renderer_images; fonts=scene.renderer_fonts;
    return true;
  }
  sk_sp<SkPicture> Picture(const sk_sp<SkData>& data) {
    if (!data) return {};
    SkDeserialProcs procedures;
    procedures.fImageCtx=this; procedures.fTypefaceCtx=this;
    procedures.fImageProc=[](const void* bytes,size_t size,std::optional<SkAlphaType>,void* context) -> sk_sp<SkImage> {
      auto& images=static_cast<Resources*>(context)->images; uint32_t index=0;
      if (size!=4) return {}; std::memcpy(&index,bytes,4); return index<images.size() ? images[index] : nullptr;
    };
    procedures.fTypefaceStreamProc=[](SkStream& stream,void* context) -> sk_sp<SkTypeface> {
      auto& fonts=static_cast<Resources*>(context)->fonts; uint32_t index=0;
      if (stream.read(&index,4)!=4) return {}; return index<fonts.size() ? fonts[index] : nullptr;
    };
    return SkPicture::MakeFromData(data->data(),data->size(),&procedures);
  }
};
struct RasterGrid {
  int left,top,right,bottom;
  explicit RasterGrid(SkIRect bounds)
      : left(std::floor(bounds.left()/512.0)),top(std::floor(bounds.top()/256.0)),
        right(std::ceil(bounds.right()/512.0)),bottom(std::ceil(bounds.bottom()/256.0)) {
    if (bounds.isEmpty()) right=left,bottom=top;
  }
  unsigned Count() const { return (right-left)*(bottom-top); }
  SkIRect Extent(SkIRect bounds,int x,int y) const {
    auto extent=SkIRect::MakeXYWH(x*512,y*256,512,256); extent.intersect(bounds); return extent;
  }
};
bool Inflate(const SkData& source,void* target,size_t size) {
  uLongf output=size; uLong input=source.size();
  return uncompress2(static_cast<Bytef*>(target),&output,source.bytes(),&input)==Z_OK &&
      output==size && input==source.size();
}
struct RasterKey { const SkData* data; SkRect bounds; };
struct Encoder {
  Writer writer;
  size_t pixels=0,tiles=0;
  std::vector<RasterKey> keys;
  void Raster(Resources& resources,const sk_sp<SkData>& data,SkRect bounds) {
    auto found=std::find_if(keys.begin(),keys.end(),[&](const auto& key){return key.data==data.get() && key.bounds==bounds;});
    unsigned reference=found==keys.end() ? 0 : unsigned(found-keys.begin())+1; Number(writer,reference);
    if (reference || !writer.valid) return;
    auto picture=resources.Picture(data);
    if (!picture || keys.size()>=4096) { std::puts("COMPOSITOR: wire raster source unavailable"); writer.valid=false; return; }
    SkIRect extent; bounds.roundOut(&extent);
    int width=extent.width(),height=extent.height(),x=extent.left(),y=extent.top();
    size_t bytes=size_t(std::max(width,0))*std::max(height,0)*4;
    RasterGrid grid(extent);
    if (width<0 || height<0 || width>8192 || height>32768 || bytes>kPixelBudget-pixels || grid.Count()>4096-tiles) {
      std::printf("COMPOSITOR: wire raster limit width=%d height=%d pixels=%zu tiles=%zu\n",width,height,pixels+bytes,tiles+grid.Count());
      writer.valid=false; return;
    }
    pixels+=bytes; tiles+=grid.Count(); Fields(writer,x,y,width,height);
    keys.push_back({data.get(),bounds});
    // Rasterization never allocates a bitmap proportional to document height.
    for (int row=grid.top;row<grid.bottom && writer.valid;++row) for (int column=grid.left;column<grid.right && writer.valid;++column) {
      auto tile=grid.Extent(extent,column,row);
      auto surface=SkSurfaces::Raster(SkImageInfo::MakeN32Premul(tile.width(),tile.height()));
      if (!surface) { writer.valid=false; return; }
      auto* canvas=surface->getCanvas(); canvas->clear(SK_ColorTRANSPARENT);
      canvas->translate(-tile.left(),-tile.top()); canvas->drawPicture(picture);
      SkPixmap bitmap; if (!surface->peekPixels(&bitmap)) { writer.valid=false; return; }
      size_t raw_size=size_t(tile.width())*tile.height()*4;
      if (bitmap.rowBytes()!=size_t(tile.width())*4) { writer.valid=false; return; }
      uLongf compressed_size=compressBound(raw_size);
      std::vector<uint8_t> compressed(compressed_size);
      if (compress2(compressed.data(),&compressed_size,static_cast<const Bytef*>(bitmap.addr()),raw_size,Z_BEST_SPEED)!=Z_OK) {
        writer.valid=false; return;
      }
      unsigned length=static_cast<unsigned>(compressed_size); Number(writer,length);
      if (!writer.Reserve(length)) return;
      writer.bytes.insert(writer.bytes.end(),compressed.begin(),compressed.begin()+length);
    }
  }
  void Scene(const ScrollScene& source) {
    ScrollScene metadata=source; Metadata(writer,metadata);
    if (!writer.valid || !Validate(metadata,true)) { std::puts("COMPOSITOR: wire metadata unsupported"); writer.valid=false; return; }
    Resources resources; if (!resources.Load(source)) { std::puts("COMPOSITOR: wire resources unavailable"); writer.valid=false; return; }
    for (const auto& picture:source.pictures) {
      unsigned count=static_cast<unsigned>(std::max(size_t(1),picture.appearances.size())); Number(writer,count);
      for (unsigned index=0;index<count && writer.valid;++index) {
        auto data=picture.appearances.empty() || !picture.appearances[index] ? picture.data : picture.appearances[index];
        auto bounds=picture.appearance_bounds.empty() ? picture.bounds : picture.appearance_bounds[index];
        Raster(resources,data,bounds);
      }
    }
  }
};
struct Decoder {
  Reader reader;
  size_t pixels=0,tiles=0;
  std::vector<std::shared_ptr<ScrollRaster>> rasters;
  std::shared_ptr<ScrollRaster> Raster() {
    unsigned reference=0; Number(reader,reference);
    if (!reader.valid) return {};
    if (reference) { if (reference>rasters.size()) { reader.valid=false; return {}; } return rasters[reference-1]; }
    int x=0,y=0,width=0,height=0; Fields(reader,x,y,width,height);
    if (!reader.valid || width<0 || height<0 || width>8192 || height>32768 || rasters.size()>=4096) { reader.valid=false; return {}; }
    size_t bytes=size_t(width)*height*4;
    auto raster=std::make_shared<ScrollRaster>(); raster->bounds=SkIRect::MakeXYWH(x,y,width,height);
    RasterGrid grid(raster->bounds);
    if (bytes>kPixelBudget-pixels || grid.Count()>4096-tiles) { reader.valid=false; return {}; }
    pixels+=bytes; tiles+=grid.Count();
    auto scratch=SkData::MakeUninitialized(512*256*4);
    for (int row=grid.top;row<grid.bottom && reader.valid;++row) for (int column=grid.left;column<grid.right && reader.valid;++column) {
      auto extent=grid.Extent(raster->bounds,column,row);
      size_t raw_size=size_t(extent.width())*extent.height()*4;
      unsigned length=0; Number(reader,length);
      if (!reader.valid || !length || length>compressBound(raw_size) || length>reader.size-reader.position) {
        reader.valid=false; return {};
      }
      auto compressed=SkData::MakeWithCopy(reader.bytes+reader.position,length); reader.position+=length;
      // Validate every stream before installation, including exact input and output lengths.
      if (!Inflate(*compressed,scratch->writable_data(),raw_size)) { reader.valid=false; return {}; }
      raster->tiles.push_back(std::move(compressed));
    }
    if (!reader.valid) return {};
    rasters.push_back(raster); return raster;
  }
  std::shared_ptr<ScrollScene> Scene() {
    auto scene=std::make_shared<ScrollScene>(); Metadata(reader,*scene);
    // All graph references are checked before any compositor operation can follow them.
    if (!reader.valid || !Validate(*scene)) { reader.valid=false; return {}; }
    for (auto& picture:scene->pictures) {
      unsigned count=0; Number(reader,count);
      size_t expected=picture.scrollbar<0 ? 1 : scene->scrollbars[picture.scrollbar].layouts.empty() ? 8 : scene->scrollbars[picture.scrollbar].layouts.size();
      if (!reader.valid || count!=expected || count>reader.records) { reader.valid=false; return {}; }
      reader.records-=count;
      for (unsigned index=0;index<count && reader.valid;++index) picture.raster_appearances.push_back(Raster());
    }
    if (!reader.valid) return {};
    return scene;
  }
};
}
std::vector<uint8_t> EncodeScrollScene(const ScrollScene& source) {
  if (!ScrollWireMemoryAvailable(8*1024*1024)) { std::puts("COMPOSITOR: wire heap headroom exhausted"); return {}; }
  Encoder encoder;
  unsigned magic=kMagic,version=kVersion,count=static_cast<unsigned>(source.reflows.size());
  Fields(encoder.writer,magic,version,count);
  if (count>32 || source.pictures.empty()) return {};
  encoder.Scene(source);
  for (const auto& scene:source.reflows) {
    if (!scene || !scene->reflows.empty() || !encoder.writer.valid) return {};
    encoder.Scene(*scene);
  }
  return encoder.writer.valid ? std::move(encoder.writer.bytes) : std::vector<uint8_t>{};
}
std::shared_ptr<ScrollScene> DecodeScrollScene(const uint8_t* bytes,size_t size) {
  if (!bytes || size<12 || size>kScrollWireCapacity || !ScrollWireMemoryAvailable(size+16*1024*1024)) return {};
  Decoder decoder{{true,bytes,size}};
  unsigned magic=0,version=0,count=0; Fields(decoder.reader,magic,version,count);
  if (magic!=kMagic || version!=kVersion || count>32) return {};
  auto scene=decoder.Scene();
  for (unsigned index=0;index<count && decoder.reader.valid;++index) {
    auto child=decoder.Scene(); if (child && scene) scene->reflows.push_back(std::move(child));
  }
  if (!decoder.reader.valid || decoder.reader.position!=size || !scene || scene->pictures.empty()) return {};
  return scene;
}
sk_sp<SkImage> DecodeScrollTile(const ScrollRaster& raster,int x,int y) {
  RasterGrid grid(raster.bounds);
  if (x<grid.left || x>=grid.right || y<grid.top || y>=grid.bottom) return {};
  size_t index=size_t(y-grid.top)*(grid.right-grid.left)+x-grid.left;
  if (index>=raster.tiles.size()) return {};
  auto extent=grid.Extent(raster.bounds,x,y);
  auto pixels=SkData::MakeUninitialized(size_t(extent.width())*extent.height()*4);
  if (!Inflate(*raster.tiles[index],pixels->writable_data(),pixels->size())) return {};
  return SkImages::RasterFromData(SkImageInfo::MakeN32Premul(extent.width(),extent.height()),pixels,size_t(extent.width())*4);
}
bool TestScrollWire() {
  ScrollScene scene;
  ScrollTransform transform; transform.element=1; transform.maximum=100; transform.viewport=64; transform.vertical=true;
  scene.transforms.push_back(transform);
  SkPictureRecorder recorder; auto* canvas=recorder.beginRecording(64,164); canvas->clear(SK_ColorRED);
  ScrollPicture picture; picture.bounds=SkRect::MakeWH(64,164); picture.transform=0;
  picture.data=recorder.finishRecordingAsPicture()->serialize(); scene.pictures.push_back(picture);
  auto encoded=EncodeScrollScene(scene); auto decoded=DecodeScrollScene(encoded.data(),encoded.size());
  if (!decoded || decoded->pictures.size()!=1 || decoded->pictures[0].raster_appearances.size()!=1) return false;
  // Truncation and structural mutations exercise the same decoder used by publications.
  for (size_t size=0;size<encoded.size();++size) if (DecodeScrollScene(encoded.data(),size)) return false;
  for (size_t offset:{size_t(0),size_t(4),size_t(8),size_t(12),size_t(20),size_t(28)}) {
    auto damaged=encoded; std::fill_n(damaged.data()+offset,4,0xff);
    if (DecodeScrollScene(damaged.data(),damaged.size())) return false;
  }
  Reader locator{true,encoded.data(),encoded.size()}; locator.position=12;
  ScrollScene metadata; Metadata(locator,metadata);
  size_t raster_start=locator.position+4;
  for (auto [offset,bits]:std::initializer_list<std::pair<size_t,uint32_t>>{
      {24,0}, {24,0xfffffffeu}, {raster_start,0xffffffffu},
      {raster_start+12,8193}, {raster_start+16,32769}, {raster_start+20,0xffffffffu}}) {
    auto damaged=encoded;
    for (unsigned shift=0;shift<32;shift+=8) damaged[offset+shift/8]=bits>>shift;
    if (DecodeScrollScene(damaged.data(),damaged.size())) return false;
  }
  auto corrupt=encoded; corrupt.back()^=1;
  if (DecodeScrollScene(corrupt.data(),corrupt.size())) return false;
  auto image=DecodeScrollTile(*decoded->pictures[0].raster_appearances[0],0,0);
  if (!image || image->width()!=64 || image->height()!=164) return false;
  auto trailing=encoded; trailing.push_back(0); if (DecodeScrollScene(trailing.data(),trailing.size())) return false;
  return true;
}
}
