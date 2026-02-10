/**
 * This code is released under the
 * Apache License Version 2.0 http://www.apache.org/licenses/.
 *
 * (c) Daniel Lemire, http://lemire.me/en/
 */

#ifndef CODECFACTORY_H_
#define CODECFACTORY_H_

#include "codecs.h"

namespace FastPForLib {

typedef std::map<std::string, std::shared_ptr<IntegerCODEC>> CodecMap;

/**
 * You should have at least one factory per thread.
 */
class CODECFactory {
public:
  CODECFactory();

  std::vector<std::shared_ptr<IntegerCODEC>> allSchemes() const;

  std::vector<std::string> allNames() const;

  std::shared_ptr<IntegerCODEC> const& getFromName(std::string name) const;
private:
  CodecMap scodecmap;
};

std::unique_ptr<IntegerCODEC> fastbinarypacking8_codec();
std::unique_ptr<IntegerCODEC> fastbinarypacking16_codec();
std::unique_ptr<IntegerCODEC> fastbinarypacking32_codec();
std::unique_ptr<IntegerCODEC> BP32_codec();
std::unique_ptr<IntegerCODEC> vsencoding_codec();
std::unique_ptr<IntegerCODEC> fastpfor128_codec();
std::unique_ptr<IntegerCODEC> fastpfor256_codec();
std::unique_ptr<IntegerCODEC> simdfastpfor128_codec();
std::unique_ptr<IntegerCODEC> simdfastpfor256_codec();
std::unique_ptr<IntegerCODEC> simplepfor_codec();
std::unique_ptr<IntegerCODEC> simdsimplepfor_codec();
std::unique_ptr<IntegerCODEC> pfor_codec();
std::unique_ptr<IntegerCODEC> simdpfor_codec();
std::unique_ptr<IntegerCODEC> pfor2008_codec();
std::unique_ptr<IntegerCODEC> simdnewpfor_codec();
std::unique_ptr<IntegerCODEC> newpfor_codec();
std::unique_ptr<IntegerCODEC> optpfor_codec();
std::unique_ptr<IntegerCODEC> simdoptpfor_codec();
std::unique_ptr<IntegerCODEC> varint_codec();
std::unique_ptr<IntegerCODEC> vbyte_codec();
std::unique_ptr<IntegerCODEC> maskedvbyte_codec();
std::unique_ptr<IntegerCODEC> streamvbyte_codec();
std::unique_ptr<IntegerCODEC> varintgb_codec();
std::unique_ptr<IntegerCODEC> simple16_codec();
std::unique_ptr<IntegerCODEC> simple9_codec();
std::unique_ptr<IntegerCODEC> simple9_rle_codec();
std::unique_ptr<IntegerCODEC> simple8b_codec();
std::unique_ptr<IntegerCODEC> simple8b_rle_codec();
#ifdef VARINTG8IU_H__
std::unique_ptr<IntegerCODEC> varintg8iu_codec();
#endif
#ifdef USESNAPPY
std::unique_ptr<IntegerCODEC> snappy_codec();
#endif
std::unique_ptr<IntegerCODEC> simdbinarypacking_codec();
std::unique_ptr<IntegerCODEC> simdgroupsimple_codec();
std::unique_ptr<IntegerCODEC> simdgroupsimple_ringbuf_codec();
std::unique_ptr<IntegerCODEC> copy_codec();

} // namespace FastPForLib

#endif /* CODECFACTORY_H_ */
