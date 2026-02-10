#include "codecfactory.h"
#include "common.h"
#include "codecs.h"
#include "vsencoding.h"
#include "util.h"
#include "simple16.h"
#include "simple9.h"
#include "simple9_rle.h"
#include "simple8b.h"
#include "simple8b_rle.h"
#include "newpfor.h"
#include "simdnewpfor.h"
#include "optpfor.h"
#include "simdoptpfor.h"
#include "fastpfor.h"
#include "simdfastpfor.h"
#include "variablebyte.h"
#include "compositecodec.h"
#include "blockpacking.h"
#include "pfor.h"
#include "simdpfor.h"
#include "pfor2008.h"
#include "VarIntG8IU.h"
#include "simdbinarypacking.h"
#include "snappydelta.h"
#include "varintgb.h"
#include "simdvariablebyte.h"
#include "streamvariablebyte.h"
#include "simdgroupsimple.h"

namespace FastPForLib {
std::vector<std::shared_ptr<IntegerCODEC>> CODECFactory::allSchemes() const {
  std::vector<std::shared_ptr<IntegerCODEC>> ans;
  for (auto i = scodecmap.begin(); i != scodecmap.end(); ++i) {
    ans.push_back(i->second);
  }
  return ans;
}

std::vector<std::string> CODECFactory::allNames() const {
  std::vector<std::string> ans;
  for (auto i = scodecmap.begin(); i != scodecmap.end(); ++i) {
    ans.push_back(i->first);
  }
  return ans;
}

std::shared_ptr<IntegerCODEC> const& CODECFactory::getFromName(std::string name) const {
  auto it = scodecmap.find(name);
  if (it != scodecmap.end())
    return it->second;

  fprintf(stderr, "name %s does not refer to a CODEC.\n"
                  "possible choices:\n", name.c_str());
  for (auto i = scodecmap.begin(); i != scodecmap.end(); ++i) {
    fprintf(stderr, "%s\n", i->first.c_str());
  }
  fprintf(stderr, "for now, I'm going to just return 'copy'\n");
  return scodecmap.at("copy");
}

// std::make_unique equivalent
template<typename T, typename... Args>
static std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

std::unique_ptr<IntegerCODEC> fastbinarypacking8_codec() {
  return make_unique<CompositeCodec<FastBinaryPacking<8>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> fastbinarypacking16_codec() {
  return make_unique<CompositeCodec<FastBinaryPacking<16>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> fastbinarypacking32_codec() {
  return make_unique<CompositeCodec<FastBinaryPacking<32>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> BP32_codec() {
  return make_unique<CompositeCodec<BP32, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> vsencoding_codec() {
  return make_unique<vsencoding::VSEncodingBlocks>(1U << 16);
}
std::unique_ptr<IntegerCODEC> fastpfor128_codec() {
  return make_unique<CompositeCodec<FastPFor<4>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> fastpfor256_codec() {
  return make_unique<CompositeCodec<FastPFor<8>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdfastpfor128_codec() {
  return make_unique<CompositeCodec<SIMDFastPFor<4>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdfastpfor256_codec() {
  return make_unique<CompositeCodec<SIMDFastPFor<8>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simplepfor_codec() {
  return make_unique<CompositeCodec<SimplePFor<>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdsimplepfor_codec() {
  return make_unique<CompositeCodec<SIMDSimplePFor<>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> pfor_codec() {
  return make_unique<CompositeCodec<PFor, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdpfor_codec() {
  return make_unique<CompositeCodec<SIMDPFor, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> pfor2008_codec() {
  return make_unique<CompositeCodec<PFor2008, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdnewpfor_codec() {
  return make_unique<CompositeCodec<SIMDNewPFor<4, Simple16<false>>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> newpfor_codec() {
  return make_unique<CompositeCodec<NewPFor<4, Simple16<false>>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> optpfor_codec() {
  return make_unique<CompositeCodec<OPTPFor<4, Simple16<false>>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdoptpfor_codec() {
  return make_unique<CompositeCodec<SIMDOPTPFor<4, Simple16<false>>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> varint_codec() {
  return make_unique<VariableByte>();
}
std::unique_ptr<IntegerCODEC> vbyte_codec() {
  return make_unique<VByte>();
}
std::unique_ptr<IntegerCODEC> maskedvbyte_codec() {
  return make_unique<MaskedVByte>();
}
std::unique_ptr<IntegerCODEC> streamvbyte_codec() {
  return make_unique<StreamVByte>();
}
std::unique_ptr<IntegerCODEC> varintgb_codec() {
  return make_unique<VarIntGB<>>();
}
std::unique_ptr<IntegerCODEC> simple16_codec() {
  return make_unique<Simple16<true>>();
}
std::unique_ptr<IntegerCODEC> simple9_codec() {
  return make_unique<Simple9<true>>();
}
std::unique_ptr<IntegerCODEC> simple9_rle_codec() {
  return make_unique<Simple9_RLE<true>>();
}
std::unique_ptr<IntegerCODEC> simple8b_codec() {
  return make_unique<Simple8b<true>>();
}
std::unique_ptr<IntegerCODEC> simple8b_rle_codec() {
  return make_unique<Simple8b_RLE<true>>();
}
#ifdef VARINTG8IU_H__
std::unique_ptr<IntegerCODEC> varintg8iu_codec() {
  return make_unique<VarIntG8IU>();
}
#endif
#ifdef USESNAPPY
std::unique_ptr<IntegerCODEC> snappy_codec() {
  return make_unique<JustSnappy>();
}
#endif
std::unique_ptr<IntegerCODEC> simdbinarypacking_codec() {
  return make_unique<CompositeCodec<SIMDBinaryPacking, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdgroupsimple_codec() {
  return make_unique<CompositeCodec<SIMDGroupSimple<false, false>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> simdgroupsimple_ringbuf_codec() {
  return make_unique<CompositeCodec<SIMDGroupSimple<true, true>, VariableByte>>();
}
std::unique_ptr<IntegerCODEC> copy_codec() {
  return make_unique<JustCopy>();
}

static CodecMap initializefactory() {
  CodecMap map;
  map["fastbinarypacking8"] = fastbinarypacking8_codec();
  map["fastbinarypacking16"] = fastbinarypacking16_codec();
  map["fastbinarypacking32"] = fastbinarypacking32_codec();
  map["BP32"] = BP32_codec();
  map["vsencoding"] = vsencoding_codec();
  map["fastpfor128"] = fastpfor128_codec();
  map["fastpfor256"] = fastpfor256_codec();
  map["simdfastpfor128"] = simdfastpfor128_codec();
  map["simdfastpfor256"] = simdfastpfor256_codec();
  map["simplepfor"] = simplepfor_codec();
  map["simdsimplepfor"] = simdsimplepfor_codec();
  map["pfor"] = pfor_codec();
  map["simdpfor"] = simdpfor_codec();
  map["pfor2008"] = pfor2008_codec();
  map["simdnewpfor"] = simdnewpfor_codec();
  map["newpfor"] = newpfor_codec();
  map["optpfor"] = optpfor_codec();
  map["simdoptpfor"] = simdoptpfor_codec();
  map["varint"] = varint_codec();
  map["vbyte"] = vbyte_codec();
  map["maskedvbyte"] = maskedvbyte_codec();
  map["streamvbyte"] = streamvbyte_codec();
  map["varintgb"] = varintgb_codec();
  map["simple16"] = simple16_codec();
  map["simple9"] = simple9_codec();
  map["simple9_rle"] = simple9_rle_codec();
  map["simple8b"] = simple8b_codec();
  map["simple8b_rle"] = simple8b_rle_codec();
#ifdef VARINTG8IU_H__
  map["varintg8iu"] = varintg8iu_codec();
#endif
#ifdef USESNAPPY
  map["snappy"] = snappy_codec();
#endif
  map["simdbinarypacking"] = simdbinarypacking_codec();
  map["simdgroupsimple"] = simdgroupsimple_codec();
  map["simdgroupsimple_ringbuf"] = simdgroupsimple_ringbuf_codec();
  map["copy"] = copy_codec();
  return map;
}

CODECFactory::CODECFactory() : scodecmap(initializefactory()) {}

} // namespace FastPForLib
