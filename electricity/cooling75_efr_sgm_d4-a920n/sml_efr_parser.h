#ifndef SML_EFR_PARSER_H
#define SML_EFR_PARSER_H

#include <stddef.h>
#include <stdint.h>

struct SmlReading {
  int64_t raw;
  int8_t scaler;
  uint8_t unit;
  bool isSigned;
};

namespace sml_efr_detail {

struct Tlv {
  uint8_t type;
  size_t data;
  size_t dataLength;
  size_t end;
  size_t items;
};

static inline bool readTlv(const uint8_t* bytes, size_t length, size_t position, Tlv* out) {
  if (!bytes || !out || position >= length) return false;

  size_t cursor = position;
  size_t tlBytes = 0;
  size_t encodedLength = 0;
  uint8_t type = 0;
  bool first = true;
  bool more = false;

  do {
    if (cursor >= length || tlBytes >= 4) return false;
    const uint8_t value = bytes[cursor++];
    if (first) {
      type = value & 0x70;
      first = false;
    } else if ((value & 0x70) != 0) {
      return false;
    }
    encodedLength = (encodedLength << 4) | (value & 0x0f);
    more = (value & 0x80) != 0;
    ++tlBytes;
  } while (more);

  out->type = type;
  out->data = cursor;
  out->items = 0;

  if (type == 0x70) {
    out->dataLength = 0;
    out->items = encodedLength;
    out->end = cursor;
    return true;
  }

  if (encodedLength < tlBytes) return false;
  const size_t payloadLength = encodedLength - tlBytes;
  if (payloadLength > length - cursor) return false;

  out->dataLength = payloadLength;
  out->end = cursor + payloadLength;
  return true;
}

static inline bool skipElement(const uint8_t* bytes, size_t length, size_t* position, uint8_t depth = 0) {
  if (!position || depth > 8) return false;
  Tlv field{};
  if (!readTlv(bytes, length, *position, &field)) return false;

  size_t cursor = field.end;
  if (field.type == 0x70) {
    cursor = field.data;
    for (size_t i = 0; i < field.items; ++i) {
      if (!skipElement(bytes, length, &cursor, static_cast<uint8_t>(depth + 1))) return false;
    }
  }

  *position = cursor;
  return true;
}

static inline bool readInteger(const uint8_t* bytes, size_t length, size_t* position,
                               int64_t* result, bool* signedValue) {
  if (!position || !result || !signedValue) return false;
  Tlv field{};
  if (!readTlv(bytes, length, *position, &field)) return false;
  if ((field.type != 0x50 && field.type != 0x60) || field.dataLength == 0 || field.dataLength > 8) {
    return false;
  }

  uint64_t raw = 0;
  for (size_t i = 0; i < field.dataLength; ++i) raw = (raw << 8) | bytes[field.data + i];

  const bool isSigned = field.type == 0x50;
  if (isSigned && field.dataLength < 8 && (bytes[field.data] & 0x80)) {
    raw |= (~UINT64_C(0)) << (field.dataLength * 8);
  }

  *result = static_cast<int64_t>(raw);
  *signedValue = isSigned;
  *position = field.end;
  return true;
}

}  // namespace sml_efr_detail

static inline bool smlFindReading(const uint8_t* bytes, size_t length,
                                  uint8_t c, uint8_t d, uint8_t e,
                                  SmlReading* reading) {
  if (!bytes || !reading || length < 8) return false;
  const uint8_t pattern[] = {0x77, 0x07, 0x01, 0x00, c, d, e, 0xff};

  for (size_t start = 0; start + sizeof(pattern) <= length; ++start) {
    bool match = true;
    for (size_t i = 0; i < sizeof(pattern); ++i) {
      if (bytes[start + i] != pattern[i]) {
        match = false;
        break;
      }
    }
    if (!match) continue;

    size_t position = start + sizeof(pattern);
    // SML_ListEntry fields after objName: status, valTime, unit, scaler, value, valueSignature.
    if (!sml_efr_detail::skipElement(bytes, length, &position)) continue;
    if (!sml_efr_detail::skipElement(bytes, length, &position)) continue;

    int64_t unit = 0;
    bool unitSigned = false;
    if (!sml_efr_detail::readInteger(bytes, length, &position, &unit, &unitSigned) ||
        unitSigned || unit < 0 || unit > 255) {
      continue;
    }

    int64_t scaler = 0;
    bool scalerSigned = false;
    if (!sml_efr_detail::readInteger(bytes, length, &position, &scaler, &scalerSigned) ||
        !scalerSigned || scaler < -128 || scaler > 127) {
      continue;
    }

    int64_t raw = 0;
    bool valueSigned = false;
    if (!sml_efr_detail::readInteger(bytes, length, &position, &raw, &valueSigned)) continue;

    reading->raw = raw;
    reading->scaler = static_cast<int8_t>(scaler);
    reading->unit = static_cast<uint8_t>(unit);
    reading->isSigned = valueSigned;
    return true;
  }
  return false;
}

static inline double smlScaledValue(const SmlReading& reading) {
  double value = static_cast<double>(reading.raw);
  if (reading.scaler > 0) {
    for (int8_t i = 0; i < reading.scaler; ++i) value *= 10.0;
  } else {
    for (int8_t i = 0; i > reading.scaler; --i) value /= 10.0;
  }
  return value;
}

#endif  // SML_EFR_PARSER_H
