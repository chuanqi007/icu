// © 2017 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

// utrie3.cpp (modified from utrie2.cpp)
// created: 2017dec29 Markus W. Scherer

#define UTRIE3_DEBUG  // TODO
#ifdef UTRIE3_DEBUG
#   include <stdio.h>
#endif

#include "unicode/utypes.h"
#include "unicode/utf.h"
#include "unicode/utf8.h"
#include "unicode/utf16.h"
#include "cmemory.h"
#include "utrie3.h"
#include "utrie3_impl.h"
#include "uassert.h"

/* Public UTrie3 API implementation ----------------------------------------- */

U_CAPI UTrie3 * U_EXPORT2
utrie3_openFromSerialized(UTrie3ValueBits valueBits,
                          const void *data, int32_t length, int32_t *pActualLength,
                          UErrorCode *pErrorCode) {
    if(U_FAILURE(*pErrorCode)) {
        return 0;
    }

    if( length<=0 || (U_POINTER_MASK_LSB(data, 3)!=0) ||
        valueBits < 0 || UTRIE3_32_VALUE_BITS < valueBits
    ) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    /* enough data for a trie header? */
    if(length<(int32_t)sizeof(UTrie3Header)) {
        *pErrorCode=U_INVALID_FORMAT_ERROR;
        return 0;
    }

    /* check the signature */
    const UTrie3Header *header=(const UTrie3Header *)data;
    if(header->signature!=UTRIE3_SIG) {
        *pErrorCode=U_INVALID_FORMAT_ERROR;
        return 0;
    }

    uint32_t options = header->options;
    if (valueBits != (UTrie3ValueBits)(options & UTRIE3_OPTIONS_VALUE_BITS_MASK) ||
            (options & UTRIE3_OPTIONS_RESERVED_MASK) != 0) {
        *pErrorCode = U_INVALID_FORMAT_ERROR;
        return 0;
    }

    /* get the length values and offsets */
    UTrie3 tempTrie;
    uprv_memset(&tempTrie, 0, sizeof(tempTrie));
    tempTrie.indexLength=header->indexLength;
    tempTrie.dataLength=header->shiftedDataLength<<UTRIE3_INDEX_SHIFT;
    tempTrie.index2NullOffset = header->index2NullOffset;
    tempTrie.dataNullOffset = options >> 12;

    tempTrie.highStart=header->shiftedHighStart<<UTRIE3_SHIFT_1;
    tempTrie.shiftedHighStart=header->shiftedHighStart;
    tempTrie.highValue=header->highValue;
    tempTrie.errorValue=header->errorValue;

    /* calculate the actual length */
    int32_t actualLength=(int32_t)sizeof(UTrie3Header)+tempTrie.indexLength*2;
    if(valueBits==UTRIE3_16_VALUE_BITS) {
        actualLength+=tempTrie.dataLength*2;
    } else {
        actualLength+=tempTrie.dataLength*4;
    }
    if(length<actualLength) {
        *pErrorCode=U_INVALID_FORMAT_ERROR;  /* not enough bytes */
        return 0;
    }

    /* allocate the trie */
    UTrie3 *trie=(UTrie3 *)uprv_malloc(sizeof(UTrie3));
    if(trie==NULL) {
        *pErrorCode=U_MEMORY_ALLOCATION_ERROR;
        return 0;
    }
    uprv_memcpy(trie, &tempTrie, sizeof(tempTrie));
    trie->name="fromSerialized";

    /* set the pointers to its index and data arrays */
    const uint16_t *p16=(const uint16_t *)(header+1);
    trie->index=p16;
    p16+=trie->indexLength;

    /* get the data */
    switch(valueBits) {
    case UTRIE3_16_VALUE_BITS:
        trie->data16=p16;
        trie->data32=NULL;
        if (trie->dataNullOffset < (trie->indexLength + trie->dataLength)) {
            trie->initialValue = trie->index[trie->dataNullOffset];
        } else {
            trie->initialValue = trie->highValue;
        }
        break;
    case UTRIE3_32_VALUE_BITS:
        trie->data16=NULL;
        trie->data32=(const uint32_t *)p16;
        if (trie->dataNullOffset < trie->dataLength) {
            trie->initialValue=trie->data32[trie->dataNullOffset];
        } else {
            trie->initialValue = trie->highValue;
        }
        break;
    default:
        *pErrorCode=U_INVALID_FORMAT_ERROR;
        return 0;
    }

    if(pActualLength!=NULL) {
        *pActualLength=actualLength;
    }
    return trie;
}

U_CAPI UTrie3 * U_EXPORT2
utrie3_clone(const UTrie3 *other, UErrorCode *pErrorCode) {
    if (U_FAILURE(*pErrorCode)) {
        return nullptr;
    }
    if (other == nullptr) {
        *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
        return nullptr;
    }

    int32_t length = other->indexLength * 2;
    if (other->data16 != nullptr) {
        length += other->dataLength * 2;
    } else {
        length += other->dataLength * 4;
    }

    char *bytes = (char *)uprv_malloc(sizeof(UTrie3) + length);
    if (bytes == nullptr) {
        *pErrorCode = U_MEMORY_ALLOCATION_ERROR;
        return nullptr;
    }
    UTrie3 *trie = (UTrie3 *)bytes;
    uprv_memcpy(trie, other, sizeof(UTrie3));
    bytes += sizeof(UTrie3);

    // Make the clone's pointers point to its own memory.
    trie->index = (uint16_t *)bytes;
    uprv_memcpy(bytes, other->index, trie->indexLength * 2);
    bytes += trie->indexLength * 2;

    if (other->data16 != nullptr) {
        trie->data16 = (uint16_t *)bytes;
        uprv_memcpy(bytes, other->data16, trie->dataLength * 2);
    } else {
        trie->data32 = (uint32_t *)bytes;
        uprv_memcpy(bytes, other->data32, trie->dataLength * 4);
    }
    return trie;
}

U_CAPI void U_EXPORT2
utrie3_close(UTrie3 *trie) {
    uprv_free(trie);
}

U_CAPI uint32_t U_EXPORT2
utrie3_get(const UTrie3 *trie, UChar32 c) {
    if ((uint32_t)c <= 0x7f) {
        // linear ASCII
        if (trie->data16 != nullptr) {
            return trie->data16[c];
        } else {
            return trie->data32[c];
        }
    }

    int32_t dataIndex;
    if ((uint32_t)c <= 0xffff) {
        dataIndex = _UTRIE3_INDEX_FROM_BMP(trie->index, c);
    } else if ((uint32_t)c > 0x10ffff) {
        return trie->errorValue;
    } else if (c >= trie->highStart) {
        return trie->highValue;
    } else {
        int32_t i2Block, dataBlock;
        dataIndex = _UTRIE3_INDEX_FROM_SUPP(trie->index, c, i2Block, dataBlock);
    }
    if (trie->data32 == nullptr) {
        return trie->index[dataIndex];
    } else {
        return trie->data32[dataIndex];
    }
}

namespace {

constexpr int32_t MAX_UNICODE = 0x10ffff;

inline uint32_t maybeHandleValue(uint32_t value, uint32_t initialValue, uint32_t nullValue,
                                 UTrie3HandleValue *handleValue, const void *context) {
    if (value == initialValue) {
        value = nullValue;
    } else if (handleValue != nullptr) {
        value = handleValue(context, value);
    }
    return value;
}

}  // namespace

U_CAPI int32_t U_EXPORT2
utrie3_getRange(const UTrie3 *trie, UChar32 start,
                UTrie3HandleValue *handleValue, const void *context, uint32_t *pValue) {
    if ((uint32_t)start > MAX_UNICODE) {
        return U_SENTINEL;
    }
    if (start >= trie->highStart) {
        if (pValue != nullptr) {
            uint32_t value = trie->highValue;
            if (handleValue != nullptr) { value = handleValue(context, value); }
            *pValue = value;
        }
        return MAX_UNICODE;
    }

    uint32_t nullValue = trie->initialValue;
    if (handleValue != nullptr) { nullValue = handleValue(context, nullValue); }
    const uint16_t *index = trie->index;
    const uint32_t *data32 = trie->data32;

    int32_t prevI2Block = -1;
    int32_t prevBlock = -1;
    UChar32 c = start;
    uint32_t value;
    bool haveValue = false;
    do {
        int32_t i2Block;
        if (c <= 0xffff) {
            i2Block = (c >> UTRIE3_SHIFT_2) & ~UTRIE3_INDEX_2_MASK;
        } else {
            // Supplementary code points
            i2Block = index[(UTRIE3_INDEX_1_OFFSET - UTRIE3_OMITTED_BMP_INDEX_1_LENGTH) +
                            (c >> UTRIE3_SHIFT_1)];
            if (i2Block == prevI2Block && (c - start) >= UTRIE3_CP_PER_INDEX_1_ENTRY) {
                // The index-2 block is the same as the previous one, and filled with value.
                // Only possible for supplementary code points because the linear-BMP index
                // table creates unique i2Block values.
                U_ASSERT((c & (UTRIE3_CP_PER_INDEX_1_ENTRY - 1)) == 0);
                c += UTRIE3_CP_PER_INDEX_1_ENTRY;
                continue;
            }
        }
        prevI2Block = i2Block;
        if (i2Block == trie->index2NullOffset) {
            // This is the index-2 null block.
            if (haveValue) {
                if (nullValue != value) {
                    return c - 1;
                }
            } else {
                value = nullValue;
                if (pValue != nullptr) { *pValue = nullValue; }
                haveValue = true;
            }
            prevBlock = trie->dataNullOffset;
            c = (c + UTRIE3_CP_PER_INDEX_1_ENTRY) & ~(UTRIE3_CP_PER_INDEX_1_ENTRY - 1);
            continue;
        }
        // Enumerate data blocks for one index-2 block.
        int32_t i2 = (c >> UTRIE3_SHIFT_2) & UTRIE3_INDEX_2_MASK;
        for(; i2 < UTRIE3_INDEX_2_BLOCK_LENGTH; ++i2) {
            int32_t block = index[i2Block + i2];
            if (i2Block >= UTRIE3_INDEX_2_BMP_LENGTH) {
                block <<= UTRIE3_INDEX_SHIFT;
            }
            if (block == prevBlock && (c - start) >= UTRIE3_DATA_BLOCK_LENGTH) {
                // The block is the same as the previous one, and filled with value.
                U_ASSERT((c & UTRIE3_DATA_MASK) == 0);
                c += UTRIE3_DATA_BLOCK_LENGTH;
                continue;
            }
            prevBlock = block;
            if (block == trie->dataNullOffset) {
                // This is the data null block.
                if (haveValue) {
                    if (nullValue != value) {
                        return c - 1;
                    }
                } else {
                    value = nullValue;
                    if (pValue != nullptr) { *pValue = nullValue; }
                    haveValue = true;
                }
                c = (c + UTRIE3_DATA_BLOCK_LENGTH) & ~UTRIE3_DATA_MASK;
            } else {
                int32_t di = block + (c & UTRIE3_DATA_MASK);
                uint32_t value2 = data32 != nullptr ? data32[di] : index[di];
                value2 = maybeHandleValue(value2, trie->initialValue, nullValue, handleValue, context);
                if (haveValue) {
                    if (value2 != value) {
                        return c - 1;
                    }
                } else {
                    value = value2;
                    if (pValue != nullptr) { *pValue = value; }
                    haveValue = true;
                }
                while ((++c & UTRIE3_DATA_MASK) != 0) {
                    if (maybeHandleValue(data32 != nullptr ? data32[++di] : index[++di],
                                         trie->initialValue, nullValue,
                                         handleValue, context) != value) {
                        return c - 1;
                    }
                }
            }
        }
    } while (c < trie->highStart);
    U_ASSERT(haveValue);
    if (maybeHandleValue(trie->highValue, trie->initialValue, nullValue,
                         handleValue, context) != value) {
        return c - 1;
    } else {
        return MAX_UNICODE;
    }
}

U_CAPI int32_t U_EXPORT2
utrie3_internalU8PrevIndex(const UTrie3 *trie, UChar32 c,
                           const uint8_t *start, const uint8_t *src) {
    int32_t i, length;
    /* support 64-bit pointers by avoiding cast of arbitrary difference */
    if((src-start)<=7) {
        i=length=(int32_t)(src-start);
    } else {
        i=length=7;
        start=src-7;
    }
    c=utf8_prevCharSafeBody(start, 0, &i, c, -1);
    i=length-i;  /* number of bytes read backward from src */
    if(c>=0) {
        int32_t idx;
        if(c<=0xffff) {
            idx=_UTRIE3_INDEX_FROM_BMP(trie->index, c);
        } else if(c>=trie->highStart) {
            return -16|i;  // for highValue
        } else {
            int32_t i2Block, dataBlock;
            idx = _UTRIE3_INDEX_FROM_SUPP(trie->index, c, i2Block, dataBlock);
        }
        return (idx<<3)|i;
    } else {
        return -8|i;  // for errorValue
    }
}

U_CAPI int32_t U_EXPORT2
utrie3_serialize(const UTrie3 *trie,
                 void *data, int32_t capacity,
                 UErrorCode *pErrorCode) {
    if(U_FAILURE(*pErrorCode)) {
        return 0;
    }

    if(trie == nullptr || capacity < 0 ||
            (capacity > 0 && (data == nullptr || (U_POINTER_MASK_LSB(data, 3) != 0)))) {
        *pErrorCode = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    UTrie3ValueBits valueBits;
    int32_t length = (int32_t)sizeof(UTrie3Header) + trie->indexLength * 2;
    if (trie->data16 != nullptr) {
        valueBits = UTRIE3_16_VALUE_BITS;
        length += trie->dataLength * 2;
    } else {
        valueBits = UTRIE3_32_VALUE_BITS;
        length += trie->dataLength * 4;
    }
    if (capacity < length) {
        *pErrorCode = U_BUFFER_OVERFLOW_ERROR;
        return length;
    }

    char *bytes = (char *)data;
    UTrie3Header *header = (UTrie3Header *)bytes;
    header->signature = UTRIE3_SIG;  // "Tri3"
    header->options = ((uint32_t)trie->dataNullOffset << 12) | valueBits;
    header->indexLength = (uint16_t)trie->indexLength;
    header->shiftedDataLength = (uint16_t)(trie->dataLength >> UTRIE3_INDEX_SHIFT);
    header->index2NullOffset = trie->index2NullOffset;
    header->shiftedHighStart = trie->shiftedHighStart;
    header->highValue = trie->highValue;
    header->errorValue = trie->errorValue;
    bytes += sizeof(UTrie3Header);

    uprv_memcpy(bytes, trie->index, trie->indexLength * 2);
    bytes += trie->indexLength * 2;

    if (trie->data16 != nullptr) {
        uprv_memcpy(bytes, trie->data16, trie->dataLength * 2);
    } else {
        uprv_memcpy(bytes, trie->data32, trie->dataLength * 4);
    }
    return length;
}

// UTrie and UTrie2 signature values,
// in platform endianness and opposite endianness.
#define UTRIE_SIG       0x54726965
#define UTRIE_OE_SIG    0x65697254

#define UTRIE2_SIG      0x54726932
#define UTRIE2_OE_SIG   0x32697254

U_CAPI int32_t U_EXPORT2
utrie3_getVersion(const void *data, int32_t length, UBool anyEndianOk) {
    uint32_t signature;
    if(length<16 || data==NULL || (U_POINTER_MASK_LSB(data, 3)!=0)) {
        return 0;
    }
    signature=*(const uint32_t *)data;
    if(signature==UTRIE3_SIG) {
        return 3;
    }
    if(anyEndianOk && signature==UTRIE3_OE_SIG) {
        return 3;
    }
    if(signature==UTRIE2_SIG) {
        return 2;
    }
    if(anyEndianOk && signature==UTRIE2_OE_SIG) {
        return 2;
    }
    if(signature==UTRIE_SIG) {
        return 1;
    }
    if(anyEndianOk && signature==UTRIE_OE_SIG) {
        return 1;
    }
    return 0;
}

namespace {

#ifdef UTRIE3_DEBUG
long countInitial(const UTrie3 *trie) {
    uint32_t initialValue=trie->initialValue;
    int32_t length=trie->dataLength;
    long count=0;
    if(trie->data16!=nullptr) {
        for(int32_t i=0; i<length; ++i) {
            if(trie->data16[i]==initialValue) { ++count; }
        }
    } else {
        for(int32_t i=0; i<length; ++i) {
            if(trie->data32[i]==initialValue) { ++count; }
        }
    }
    return count;
}

U_CFUNC void
utrie3_printLengths(const UTrie3 *trie, const char *which) {
    long indexLength=trie->indexLength;
    long dataLength=(long)trie->dataLength;
    long totalLength=(long)sizeof(UTrie3Header)+indexLength*2+dataLength*(trie->data32!=NULL ? 4 : 2);
    printf("**UTrie3Lengths(%s %s)** index:%6ld  data:%6ld  countInitial:%6ld  serialized:%6ld\n",
           which, trie->name, indexLength, dataLength, countInitial(trie), totalLength);
}
#endif

}  // namespace

U_CAPI int32_t U_EXPORT2
utrie3_swap(const UDataSwapper *ds,
            const void *inData, int32_t length, void *outData,
            UErrorCode *pErrorCode) {
    const UTrie3Header *inTrie;
    UTrie3Header trie;
    int32_t dataLength, size;
    UTrie3ValueBits valueBits;

    if(U_FAILURE(*pErrorCode)) {
        return 0;
    }
    if(ds==NULL || inData==NULL || (length>=0 && outData==NULL)) {
        *pErrorCode=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    /* setup and swapping */
    if(length>=0 && length<(int32_t)sizeof(UTrie3Header)) {
        *pErrorCode=U_INDEX_OUTOFBOUNDS_ERROR;
        return 0;
    }

    inTrie=(const UTrie3Header *)inData;
    trie.signature=ds->readUInt32(inTrie->signature);
    trie.options=ds->readUInt32(inTrie->options);
    trie.indexLength=ds->readUInt16(inTrie->indexLength);
    trie.shiftedDataLength=ds->readUInt16(inTrie->shiftedDataLength);

    valueBits=(UTrie3ValueBits)(trie.options&UTRIE3_OPTIONS_VALUE_BITS_MASK);
    dataLength=(int32_t)trie.shiftedDataLength<<UTRIE3_INDEX_SHIFT;

    if( trie.signature!=UTRIE3_SIG ||
        valueBits < 0 || UTRIE3_32_VALUE_BITS < valueBits ||
        (trie.options & UTRIE3_OPTIONS_RESERVED_MASK) != 0 ||
        trie.indexLength<UTRIE3_INDEX_1_OFFSET ||
        dataLength<UTRIE3_DATA_START_OFFSET
    ) {
        *pErrorCode=U_INVALID_FORMAT_ERROR; /* not a UTrie */
        return 0;
    }

    size=sizeof(UTrie3Header)+trie.indexLength*2;
    switch(valueBits) {
    case UTRIE3_16_VALUE_BITS:
        size+=dataLength*2;
        break;
    case UTRIE3_32_VALUE_BITS:
        size+=dataLength*4;
        break;
    default:
        *pErrorCode=U_INVALID_FORMAT_ERROR;
        return 0;
    }

    if(length>=0) {
        UTrie3Header *outTrie;

        if(length<size) {
            *pErrorCode=U_INDEX_OUTOFBOUNDS_ERROR;
            return 0;
        }

        outTrie=(UTrie3Header *)outData;

        /* swap the header */
        ds->swapArray32(ds, &inTrie->signature, 8, &outTrie->signature, pErrorCode);
        ds->swapArray16(ds, &inTrie->indexLength, 8, &outTrie->indexLength, pErrorCode);
        ds->swapArray32(ds, &inTrie->highValue, 8, &outTrie->highValue, pErrorCode);

        /* swap the index and the data */
        switch(valueBits) {
        case UTRIE3_16_VALUE_BITS:
            ds->swapArray16(ds, inTrie+1, (trie.indexLength+dataLength)*2, outTrie+1, pErrorCode);
            break;
        case UTRIE3_32_VALUE_BITS:
            ds->swapArray16(ds, inTrie+1, trie.indexLength*2, outTrie+1, pErrorCode);
            ds->swapArray32(ds, (const uint16_t *)(inTrie+1)+trie.indexLength, dataLength*4,
                                     (uint16_t *)(outTrie+1)+trie.indexLength, pErrorCode);
            break;
        default:
            *pErrorCode=U_INVALID_FORMAT_ERROR;
            return 0;
        }
    }

    return size;
}

// utrie3_swapAnyVersion() should be defined here but lives in utrie3_builder.cpp
// to avoid a dependency from utrie3.cpp on utrie.cpp.

/* C++ convenience wrappers ------------------------------------------------- */

U_NAMESPACE_BEGIN

uint16_t BackwardUTrie3StringIterator::previous16() {
    codePointLimit=codePointStart;
    if(start>=codePointStart) {
        codePoint=U_SENTINEL;
        return trie->errorValue;
    }
    uint16_t result;
    UTRIE3_U16_PREV16(trie, start, codePointStart, codePoint, result);
    return result;
}

uint16_t ForwardUTrie3StringIterator::next16() {
    codePointStart=codePointLimit;
    if(codePointLimit==limit) {
        codePoint=U_SENTINEL;
        return trie->errorValue;
    }
    uint16_t result;
    UTRIE3_U16_NEXT16(trie, codePointLimit, limit, codePoint, result);
    return result;
}

U_NAMESPACE_END
