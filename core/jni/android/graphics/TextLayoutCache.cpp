/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "TextLayoutCache"

#include "TextLayoutCache.h"
#include "TextLayout.h"

extern "C" {
  #include "harfbuzz-unicode.h"
}

namespace android {

//--------------------------------------------------------------------------------------------------
#if USE_TEXT_LAYOUT_CACHE
    ANDROID_SINGLETON_STATIC_INSTANCE(TextLayoutCache);
#endif
//--------------------------------------------------------------------------------------------------

TextLayoutCache::TextLayoutCache() :
        mCache(GenerationCache<TextLayoutCacheKey, sp<TextLayoutCacheValue> >::kUnlimitedCapacity),
        mSize(0), mMaxSize(MB(DEFAULT_TEXT_LAYOUT_CACHE_SIZE_IN_MB)),
        mCacheHitCount(0), mNanosecondsSaved(0) {
    init();
}

TextLayoutCache::~TextLayoutCache() {
    mCache.clear();
}

void TextLayoutCache::init() {
    mCache.setOnEntryRemovedListener(this);

    mDebugLevel = readRtlDebugLevel();
    mDebugEnabled = mDebugLevel & kRtlDebugCaches;
    LOGD("Using debug level: %d - Debug Enabled: %d", mDebugLevel, mDebugEnabled);

    mCacheStartTime = systemTime(SYSTEM_TIME_MONOTONIC);

    if (mDebugEnabled) {
        LOGD("Initialization is done - Start time: %lld", mCacheStartTime);
    }

    mInitialized = true;
}

/*
 * Size management
 */

uint32_t TextLayoutCache::getSize() {
    return mSize;
}

uint32_t TextLayoutCache::getMaxSize() {
    return mMaxSize;
}

void TextLayoutCache::setMaxSize(uint32_t maxSize) {
    mMaxSize = maxSize;
    removeOldests();
}

void TextLayoutCache::removeOldests() {
    while (mSize > mMaxSize) {
        mCache.removeOldest();
    }
}

/**
 *  Callbacks
 */
void TextLayoutCache::operator()(TextLayoutCacheKey& text, sp<TextLayoutCacheValue>& desc) {
    if (desc != NULL) {
        size_t totalSizeToDelete = text.getSize() + desc->getSize();
        mSize -= totalSizeToDelete;
        if (mDebugEnabled) {
            LOGD("Cache value deleted, size = %d", totalSizeToDelete);
        }
        desc.clear();
    }
}

/*
 * Cache clearing
 */
void TextLayoutCache::clear() {
    mCache.clear();
}

/*
 * Caching
 */
sp<TextLayoutCacheValue> TextLayoutCache::getValue(SkPaint* paint,
            const jchar* text, jint count, jint dirFlags) {
    AutoMutex _l(mLock);
    nsecs_t startTime = 0;
    if (mDebugEnabled) {
        startTime = systemTime(SYSTEM_TIME_MONOTONIC);
    }

    // Create the key
    TextLayoutCacheKey key(paint, text, count, dirFlags);

    // Get value from cache if possible
    sp<TextLayoutCacheValue> value = mCache.get(key);

    // Value not found for the key, we need to add a new value in the cache
    if (value == NULL) {
        if (mDebugEnabled) {
            startTime = systemTime(SYSTEM_TIME_MONOTONIC);
        }

        value = new TextLayoutCacheValue();

        // Compute advances and store them
        value->computeValues(paint, text, count, dirFlags);

        nsecs_t endTime = systemTime(SYSTEM_TIME_MONOTONIC);

        // Don't bother to add in the cache if the entry is too big
        size_t size = key.getSize() + value->getSize();
        if (size <= mMaxSize) {
            // Cleanup to make some room if needed
            if (mSize + size > mMaxSize) {
                if (mDebugEnabled) {
                    LOGD("Need to clean some entries for making some room for a new entry");
                }
                while (mSize + size > mMaxSize) {
                    // This will call the callback
                    mCache.removeOldest();
                }
            }

            // Update current cache size
            mSize += size;

            // Copy the text when we insert the new entry
            key.internalTextCopy();
            mCache.put(key, value);

            if (mDebugEnabled) {
                // Update timing information for statistics
                value->setElapsedTime(endTime - startTime);

                LOGD("CACHE MISS: Added entry with "
                        "count=%d, entry size %d bytes, remaining space %d bytes"
                        " - Compute time in nanos: %d - Text='%s' ",
                        count, size, mMaxSize - mSize, value->getElapsedTime(),
                        String8(text, count).string());
            }
        } else {
            if (mDebugEnabled) {
                LOGD("CACHE MISS: Calculated but not storing entry because it is too big "
                        "with count=%d, "
                        "entry size %d bytes, remaining space %d bytes"
                        " - Compute time in nanos: %lld - Text='%s'",
                        count, size, mMaxSize - mSize, endTime,
                        String8(text, count).string());
            }
            value.clear();
        }
    } else {
        // This is a cache hit, just log timestamp and user infos
        if (mDebugEnabled) {
            nsecs_t elapsedTimeThruCacheGet = systemTime(SYSTEM_TIME_MONOTONIC) - startTime;
            mNanosecondsSaved += (value->getElapsedTime() - elapsedTimeThruCacheGet);
            ++mCacheHitCount;

            if (value->getElapsedTime() > 0) {
                float deltaPercent = 100 * ((value->getElapsedTime() - elapsedTimeThruCacheGet)
                        / ((float)value->getElapsedTime()));
                LOGD("CACHE HIT #%d with count=%d "
                        "- Compute time in nanos: %d - "
                        "Cache get time in nanos: %lld - Gain in percent: %2.2f - Text='%s' ",
                        mCacheHitCount, count,
                        value->getElapsedTime(), elapsedTimeThruCacheGet, deltaPercent,
                        String8(text, count).string());
            }
            if (mCacheHitCount % DEFAULT_DUMP_STATS_CACHE_HIT_INTERVAL == 0) {
                dumpCacheStats();
            }
        }
    }
    return value;
}

void TextLayoutCache::dumpCacheStats() {
    float remainingPercent = 100 * ((mMaxSize - mSize) / ((float)mMaxSize));
    float timeRunningInSec = (systemTime(SYSTEM_TIME_MONOTONIC) - mCacheStartTime) / 1000000000;
    LOGD("------------------------------------------------");
    LOGD("Cache stats");
    LOGD("------------------------------------------------");
    LOGD("pid       : %d", getpid());
    LOGD("running   : %.0f seconds", timeRunningInSec);
    LOGD("entries   : %d", mCache.size());
    LOGD("size      : %d bytes", mMaxSize);
    LOGD("remaining : %d bytes or %2.2f percent", mMaxSize - mSize, remainingPercent);
    LOGD("hits      : %d", mCacheHitCount);
    LOGD("saved     : %lld milliseconds", mNanosecondsSaved / 1000000);
    LOGD("------------------------------------------------");
}

/**
 * TextLayoutCacheKey
 */
TextLayoutCacheKey::TextLayoutCacheKey(): text(NULL), count(0),
        dirFlags(0), typeface(NULL), textSize(0), textSkewX(0), textScaleX(0), flags(0),
        hinting(SkPaint::kNo_Hinting)  {
}

TextLayoutCacheKey::TextLayoutCacheKey(const SkPaint* paint,
        const UChar* text, size_t count, int dirFlags) :
            text(text), count(count),
            dirFlags(dirFlags) {
    typeface = paint->getTypeface();
    textSize = paint->getTextSize();
    textSkewX = paint->getTextSkewX();
    textScaleX = paint->getTextScaleX();
    flags = paint->getFlags();
    hinting = paint->getHinting();
}

TextLayoutCacheKey::TextLayoutCacheKey(const TextLayoutCacheKey& other) :
        text(NULL),
        textCopy(other.textCopy),
        count(other.count),
        dirFlags(other.dirFlags),
        typeface(other.typeface),
        textSize(other.textSize),
        textSkewX(other.textSkewX),
        textScaleX(other.textScaleX),
        flags(other.flags),
        hinting(other.hinting) {
    if (other.text) {
        textCopy.setTo(other.text);
    }
}

bool TextLayoutCacheKey::operator<(const TextLayoutCacheKey& rhs) const {
    LTE_INT(count) {
        LTE_INT(typeface) {
            LTE_FLOAT(textSize) {
                LTE_FLOAT(textSkewX) {
                    LTE_FLOAT(textScaleX) {
                        LTE_INT(flags) {
                            LTE_INT(hinting) {
                                LTE_INT(dirFlags) {
                                    return memcmp(getText(), rhs.getText(),
                                            count * sizeof(UChar)) < 0;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

void TextLayoutCacheKey::internalTextCopy() {
    textCopy.setTo(text, count);
    text = NULL;
}

size_t TextLayoutCacheKey::getSize() {
    return sizeof(TextLayoutCacheKey) + sizeof(UChar) * count;
}

/**
 * TextLayoutCacheValue
 */
TextLayoutCacheValue::TextLayoutCacheValue() :
        mTotalAdvance(0), mElapsedTime(0) {
}

void TextLayoutCacheValue::setElapsedTime(uint32_t time) {
    mElapsedTime = time;
}

uint32_t TextLayoutCacheValue::getElapsedTime() {
    return mElapsedTime;
}

void TextLayoutCacheValue::computeValues(SkPaint* paint, const UChar* chars,
        size_t contextCount, int dirFlags) {
    // Give a hint for advances, glyphs and log clusters vectors size
    mAdvances.setCapacity(contextCount);
    mGlyphs.setCapacity(contextCount);
    mLogClusters.setCapacity(contextCount);

    computeValuesWithHarfbuzz(paint, chars, contextCount, dirFlags,
            &mAdvances, &mTotalAdvance, &mGlyphs, &mLogClusters);
#if DEBUG_ADVANCES
    LOGD("Advances - countextCount=%d - totalAdvance=%f", contextCount, mTotalAdvance);
#endif
}

size_t TextLayoutCacheValue::getSize() {
    return sizeof(TextLayoutCacheValue) + sizeof(jfloat) * mAdvances.capacity() +
            sizeof(jchar) * mGlyphs.capacity() + sizeof(unsigned short) * mLogClusters.capacity();
}

void TextLayoutCacheValue::setupShaperItem(HB_ShaperItem* shaperItem, HB_FontRec* font,
        FontData* fontData, SkPaint* paint, const UChar* chars, size_t start, size_t count,
        size_t contextCount, bool isRTL) {
    font->klass = &harfbuzzSkiaClass;
    font->userData = 0;
    // The values which harfbuzzSkiaClass returns are already scaled to
    // pixel units, so we just set all these to one to disable further
    // scaling.
    font->x_ppem = 1;
    font->y_ppem = 1;
    font->x_scale = 1;
    font->y_scale = 1;

    memset(shaperItem, 0, sizeof(*shaperItem));
    shaperItem->font = font;
    shaperItem->face = HB_NewFace(shaperItem->font, harfbuzzSkiaGetTable);

    shaperItem->kerning_applied = false;

    // We cannot know, ahead of time, how many glyphs a given script run
    // will produce. We take a guess that script runs will not produce more
    // than twice as many glyphs as there are code points plus a bit of
    // padding and fallback if we find that we are wrong.
    createGlyphArrays(shaperItem, (contextCount + 2) * 2);

    // Free memory for clusters if needed and recreate the clusters array
    if (shaperItem->log_clusters) {
        delete shaperItem->log_clusters;
    }
    shaperItem->log_clusters = new unsigned short[contextCount];

    shaperItem->item.pos = start;
    shaperItem->item.length = count;
    shaperItem->item.bidiLevel = isRTL;

    shaperItem->item.script = isRTL ? HB_Script_Arabic : HB_Script_Common;

    shaperItem->string = chars;
    shaperItem->stringLength = contextCount;

    fontData->typeFace = paint->getTypeface();
    fontData->textSize = paint->getTextSize();
    fontData->textSkewX = paint->getTextSkewX();
    fontData->textScaleX = paint->getTextScaleX();
    fontData->flags = paint->getFlags();
    fontData->hinting = paint->getHinting();

    shaperItem->font->userData = fontData;
}

void TextLayoutCacheValue::shapeWithHarfbuzz(HB_ShaperItem* shaperItem, HB_FontRec* font,
        FontData* fontData, SkPaint* paint, const UChar* chars, size_t start, size_t count,
        size_t contextCount, bool isRTL) {
    // Setup Harfbuzz Shaper
    setupShaperItem(shaperItem, font, fontData, paint, chars, start, count,
            contextCount, isRTL);

    // Shape
    resetGlyphArrays(shaperItem);
    while (!HB_ShapeItem(shaperItem)) {
        // We overflowed our arrays. Resize and retry.
        // HB_ShapeItem fills in shaperItem.num_glyphs with the needed size.
        deleteGlyphArrays(shaperItem);
        createGlyphArrays(shaperItem, shaperItem->num_glyphs << 1);
        resetGlyphArrays(shaperItem);
    }
}

void TextLayoutCacheValue::computeValuesWithHarfbuzz(SkPaint* paint, const UChar* chars,
        size_t contextCount, int dirFlags,
        Vector<jfloat>* const outAdvances, jfloat* outTotalAdvance,
        Vector<jchar>* const outGlyphs, Vector<unsigned short>* const outLogClusters) {

        UBiDiLevel bidiReq = 0;
        bool forceLTR = false;
        bool forceRTL = false;

        switch (dirFlags) {
            case kBidi_LTR: bidiReq = 0; break; // no ICU constant, canonical LTR level
            case kBidi_RTL: bidiReq = 1; break; // no ICU constant, canonical RTL level
            case kBidi_Default_LTR: bidiReq = UBIDI_DEFAULT_LTR; break;
            case kBidi_Default_RTL: bidiReq = UBIDI_DEFAULT_RTL; break;
            case kBidi_Force_LTR: forceLTR = true; break; // every char is LTR
            case kBidi_Force_RTL: forceRTL = true; break; // every char is RTL
        }

        if (forceLTR || forceRTL) {
#if DEBUG_GLYPHS
                    LOGD("computeValuesWithHarfbuzz -- forcing run with LTR=%d RTL=%d",
                            forceLTR, forceRTL);
#endif
            computeRunValuesWithHarfbuzz(paint, chars, 0, contextCount, contextCount, forceRTL,
                    outAdvances, outTotalAdvance, outGlyphs, outLogClusters);
        } else {
            UBiDi* bidi = ubidi_open();
            if (bidi) {
                UErrorCode status = U_ZERO_ERROR;
#if DEBUG_GLYPHS
                LOGD("computeValuesWithHarfbuzz -- bidiReq=%d", bidiReq);
#endif
                ubidi_setPara(bidi, chars, contextCount, bidiReq, NULL, &status);
                if (U_SUCCESS(status)) {
                    int paraDir = ubidi_getParaLevel(bidi) & kDirection_Mask; // 0 if ltr, 1 if rtl
                    size_t rc = ubidi_countRuns(bidi, &status);
#if DEBUG_GLYPHS
                    LOGD("computeValuesWithHarfbuzz -- dirFlags=%d run-count=%d paraDir=%d", dirFlags, rc, paraDir);
#endif
                    if (rc == 1 || !U_SUCCESS(status)) {
                        bool isRTL = (paraDir == 1);
#if DEBUG_GLYPHS
                        LOGD("computeValuesWithHarfbuzz -- processing SINGLE run "
                                "-- run-start=%d run-len=%d isRTL=%d", 0, contextCount, isRTL);
#endif
                        computeRunValuesWithHarfbuzz(paint, chars, 0, contextCount, contextCount,
                                isRTL, outAdvances, outTotalAdvance, outGlyphs, outLogClusters);
                    } else {
                        for (size_t i = 0; i < rc; ++i) {
                            int32_t startRun;
                            int32_t lengthRun;
                            UBiDiDirection runDir = ubidi_getVisualRun(bidi, i, &startRun, &lengthRun);

                            bool isRTL = (runDir == UBIDI_RTL);
                            jfloat runTotalAdvance = 0;
#if DEBUG_GLYPHS
                            LOGD("computeValuesWithHarfbuzz -- run-start=%d run-len=%d isRTL=%d",
                                    startRun, lengthRun, isRTL);
#endif
                            computeRunValuesWithHarfbuzz(paint, chars, startRun,
                                    lengthRun, contextCount, isRTL,
                                    outAdvances, &runTotalAdvance,
                                    outGlyphs, outLogClusters);

                            *outTotalAdvance += runTotalAdvance;
                        }
                    }
                }
                ubidi_close(bidi);
            } else {
                // Cannot run BiDi, just consider one Run
                bool isRTL = (bidiReq = 1) || (bidiReq = UBIDI_DEFAULT_RTL);
#if DEBUG_GLYPHS
                LOGD("computeValuesWithHarfbuzz -- cannot run BiDi, considering a SINGLE Run "
                        "-- run-start=%d run-len=%d isRTL=%d", 0, contextCount, isRTL);
#endif
                computeRunValuesWithHarfbuzz(paint, chars, 0, contextCount, contextCount, isRTL,
                        outAdvances, outTotalAdvance, outGlyphs, outLogClusters);
            }
        }
#if DEBUG_GLYPHS
        LOGD("computeValuesWithHarfbuzz -- total-glyphs-count=%d", outGlyphs->size());
#endif
}

static void logGlyphs(HB_ShaperItem shaperItem) {
    LOGD("Got glyphs - count=%d", shaperItem.num_glyphs);
    for (size_t i = 0; i < shaperItem.num_glyphs; i++) {
        LOGD("      glyph[%d]=%d - offset.x=%f offset.y=%f", i, shaperItem.glyphs[i],
                HBFixedToFloat(shaperItem.offsets[i].x),
                HBFixedToFloat(shaperItem.offsets[i].y));
    }
}

void TextLayoutCacheValue::computeRunValuesWithHarfbuzz(SkPaint* paint, const UChar* chars,
        size_t start, size_t count, size_t contextCount, bool isRTL,
        Vector<jfloat>* const outAdvances, jfloat* outTotalAdvance,
        Vector<jchar>* const outGlyphs, Vector<unsigned short>* const outLogClusters) {

    HB_ShaperItem shaperItem;
    HB_FontRec font;
    FontData fontData;

    shapeWithHarfbuzz(&shaperItem, &font, &fontData, paint, chars, start, count,
            contextCount, isRTL);

#if DEBUG_GLYPHS
    LOGD("HARFBUZZ -- num_glypth=%d - kerning_applied=%d", shaperItem.num_glyphs,
            shaperItem.kerning_applied);
    LOGD("         -- string= '%s'", String8(chars + start, count).string());
    LOGD("         -- isDevKernText=%d", paint->isDevKernText());

    logGlyphs(shaperItem);
#endif

    if (shaperItem.advances == NULL || shaperItem.num_glyphs == 0) {
#if DEBUG_GLYPHS
    LOGD("HARFBUZZ -- advances array is empty or num_glypth = 0");
#endif
        outAdvances->insertAt(0, outAdvances->size(), count);
        *outTotalAdvance = 0;

        // Cleaning
        deleteGlyphArrays(&shaperItem);
        HB_FreeFace(shaperItem.face);
        return;
    }

    // Get Advances and their total
    jfloat currentAdvance = HBFixedToFloat(shaperItem.advances[shaperItem.log_clusters[0]]);
    jfloat totalAdvance = currentAdvance;
    outAdvances->add(currentAdvance);
    for (size_t i = 1; i < count; i++) {
        size_t clusterPrevious = shaperItem.log_clusters[i - 1];
        size_t cluster = shaperItem.log_clusters[i];
        if (cluster == clusterPrevious) {
            outAdvances->add(0);
        } else {
            currentAdvance = HBFixedToFloat(shaperItem.advances[shaperItem.log_clusters[i]]);
            totalAdvance += currentAdvance;
            outAdvances->add(currentAdvance);
        }
    }
    *outTotalAdvance = totalAdvance;

#if DEBUG_ADVANCES
    for (size_t i = 0; i < count; i++) {
        LOGD("hb-adv[%d] = %f - log_clusters = %d - total = %f", i,
                (*outAdvances)[i], shaperItem.log_clusters[i], totalAdvance);
    }
#endif

    // Get Glyphs and reverse them in place if RTL
    if (outGlyphs) {
        size_t countGlyphs = shaperItem.num_glyphs;
        for (size_t i = 0; i < countGlyphs; i++) {
            jchar glyph = (jchar) shaperItem.glyphs[(!isRTL) ? i : countGlyphs - 1 - i];
#if DEBUG_GLYPHS
            LOGD("HARFBUZZ  -- glyph[%d]=%d", i, glyph);
#endif
            outGlyphs->add(glyph);
        }
    }

    // Get LogClusters
    if (outLogClusters) {
        size_t countLogClusters = outLogClusters->size();
        size_t countGlyphs = shaperItem.num_glyphs;
        for (size_t i = 0; i < countGlyphs; i++) {
            // As there may be successive runs, we need to shift the log clusters
            unsigned short logCluster = shaperItem.log_clusters[i] + countLogClusters;
#if DEBUG_GLYPHS
            LOGD("HARFBUZZ  -- logCluster[%d] relative=%d - absolute=%d", i, shaperItem.log_clusters[i], logCluster);
#endif
            outLogClusters->add(logCluster);
        }
    }

    // Cleaning
    deleteGlyphArrays(&shaperItem);
    HB_FreeFace(shaperItem.face);
}

void TextLayoutCacheValue::deleteGlyphArrays(HB_ShaperItem* shaperItem) {
    delete[] shaperItem->glyphs;
    delete[] shaperItem->attributes;
    delete[] shaperItem->advances;
    delete[] shaperItem->offsets;
}

void TextLayoutCacheValue::createGlyphArrays(HB_ShaperItem* shaperItem, int size) {
    shaperItem->glyphs = new HB_Glyph[size];
    shaperItem->attributes = new HB_GlyphAttributes[size];
    shaperItem->advances = new HB_Fixed[size];
    shaperItem->offsets = new HB_FixedPoint[size];
    shaperItem->num_glyphs = size;
}

void TextLayoutCacheValue::resetGlyphArrays(HB_ShaperItem* shaperItem) {
    int size = shaperItem->num_glyphs;
    // All the types here don't have pointers. It is safe to reset to
    // zero unless Harfbuzz breaks the compatibility in the future.
    memset(shaperItem->glyphs, 0, size * sizeof(shaperItem->glyphs[0]));
    memset(shaperItem->attributes, 0, size * sizeof(shaperItem->attributes[0]));
    memset(shaperItem->advances, 0, size * sizeof(shaperItem->advances[0]));
    memset(shaperItem->offsets, 0, size * sizeof(shaperItem->offsets[0]));
}

void TextLayoutCacheValue::getAdvances(size_t start, size_t count, jfloat* outAdvances) const {
    memcpy(outAdvances, mAdvances.array() + start, count * sizeof(jfloat));
#if DEBUG_ADVANCES
    LOGD("getAdvances - start=%d count=%d", start, count);
    for (size_t i = 0; i < count; i++) {
        LOGD("  adv[%d] = %f", i, outAdvances[i]);
    }
#endif
}

jfloat TextLayoutCacheValue::getTotalAdvance(size_t start, size_t count) const {
    jfloat outTotalAdvance = 0;
    for (size_t i = start; i < start + count; i++) {
        outTotalAdvance += mAdvances[i];
    }
#if DEBUG_ADVANCES
    LOGD("getTotalAdvance - start=%d count=%d - total=%f", start, count, outTotalAdvance);
#endif
     return outTotalAdvance;
}

void TextLayoutCacheValue::getGlyphsIndexAndCount(size_t start, size_t count, size_t* outStartIndex,
        size_t* outGlyphsCount) const {
    *outStartIndex = 0;
    if (count == 0) {
        *outGlyphsCount = 0;
        return;
    }
    size_t endIndex = 0;
    for(size_t i = 0; i < mGlyphs.size(); i++) {
        if (mLogClusters[i] <= start) {
            *outStartIndex = i;
            endIndex = i;
            continue;
        }
        if (mLogClusters[i] <= start + count) {
            endIndex = i;
        }
    }
    *outGlyphsCount = endIndex - *outStartIndex + 1;
#if DEBUG_GLYPHS
    LOGD("getGlyphsIndexes - start=%d count=%d - startIndex=%d count=%d", start, count,
            *outStartIndex, *outGlyphsCount);
    for(size_t i = 0; i < mGlyphs.size(); i++) {
        LOGD("getGlyphs - all - glyph[%d] = %d", i, mGlyphs[i]);
    }
    for(size_t i = 0; i < mGlyphs.size(); i++) {
        LOGD("getGlyphs - all - logcl[%d] = %d", i, mLogClusters[i]);
    }
#endif
}

const jchar* TextLayoutCacheValue::getGlyphs(size_t startIndex, size_t count) {
    const jchar* glyphs = mGlyphs.array() + startIndex;
#if DEBUG_GLYPHS
    LOGD("getGlyphs - with startIndex = %d  count = %d", startIndex, count);
    for (size_t i = 0; i < count; i++) {
        LOGD("getGlyphs - result - glyph[%d] = %d", i, glyphs[i]);
    }
#endif
    return glyphs;
}

} // namespace android
