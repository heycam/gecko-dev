/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_RuleProcessorCache_h
#define mozilla_RuleProcessorCache_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/StaticPtr.h"
#include "nsExpirationTracker.h"
#include "nsIMediaList.h"
#include "nsIMemoryReporter.h"
#include "nsTArray.h"

class nsCSSRuleProcessor;
namespace mozilla {
class CSSStyleSheet;
namespace css {
class DocumentRule;
}
}

namespace mozilla {

class RuleProcessorCache final : public nsIMemoryReporter
{
  NS_DECL_ISUPPORTS
  NS_DECL_NSIMEMORYREPORTER

public:
  static nsCSSRuleProcessor* GetRuleProcessor(
      const nsTArray<CSSStyleSheet*>& aSheets,
      nsPresContext* aPresContext);
  static void PutRuleProcessor(
      const nsTArray<CSSStyleSheet*>& aSheets,
      nsTArray<css::DocumentRule*>&& aDocumentRules,
      const nsDocumentRuleResultCacheKey& aCacheKey,
      nsCSSRuleProcessor* aRuleProcessor);
  static void StartTracking(nsCSSRuleProcessor* aRuleProcessor);
  static void StopTracking(nsCSSRuleProcessor* aRuleProcessor);

#ifdef DEBUG
  static bool HasRuleProcessor(nsCSSRuleProcessor* aRuleProcessor);
#endif
  static void RemoveRuleProcessor(nsCSSRuleProcessor* aRuleProcessor);
  static void RemoveSheet(CSSStyleSheet* aSheet);

  static void Shutdown() { gShutdown = true; gRuleProcessorCache = nullptr; }

  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf);

private:
  class ExpirationTracker : public nsExpirationTracker<nsCSSRuleProcessor,3>
  {
  public:
    ExpirationTracker(RuleProcessorCache* aCache)
      : nsExpirationTracker<nsCSSRuleProcessor,3>(10000)
      , mCache(aCache) {}

    void RemoveFromTracker(nsCSSRuleProcessor* aRuleProcessor);

    virtual void NotifyExpired(nsCSSRuleProcessor* aRuleProcessor) override {
      mCache->RemoveRuleProcessor(aRuleProcessor);
    }

  private:
    RuleProcessorCache* mCache;
  };

  RuleProcessorCache() : mExpirationTracker(this) {}
  ~RuleProcessorCache();

  void InitMemoryReporter();

  static bool EnsureGlobal();
  static StaticRefPtr<RuleProcessorCache> gRuleProcessorCache;
  static bool gShutdown;

  void DoRemoveSheet(CSSStyleSheet* aSheet);
  nsCSSRuleProcessor* DoGetRuleProcessor(
      const nsTArray<CSSStyleSheet*>& aSheets,
      nsPresContext* aPresContext);
  void DoPutRuleProcessor(const nsTArray<CSSStyleSheet*>& aSheets,
                          nsTArray<css::DocumentRule*>&& aDocumentRules,
                          const nsDocumentRuleResultCacheKey& aCacheKey,
                          nsCSSRuleProcessor* aRuleProcessor);
#ifdef DEBUG
  bool DoHasRuleProcessor(nsCSSRuleProcessor* aRuleProcessor);
#endif
  void DoRemoveRuleProcessor(nsCSSRuleProcessor* aRuleProcessor);
  void DoStartTracking(nsCSSRuleProcessor* aRuleProcessor);
  void DoStopTracking(nsCSSRuleProcessor* aRuleProcessor);

  size_t DoSizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf);

  struct DocumentEntry {
    nsDocumentRuleResultCacheKey mCacheKey;
    nsRefPtr<nsCSSRuleProcessor> mRuleProcessor;
  };

  struct Entry {
    nsTArray<CSSStyleSheet*>     mSheets;
    nsTArray<css::DocumentRule*> mDocumentRules;
    nsTArray<DocumentEntry>      mDocumentEntries;
  };

  // Function object to test whether an Entry object has a given sheet
  // in its mSheets array.
  struct HasSheet {
    HasSheet(CSSStyleSheet* aSheet) : mSheet(aSheet) {}
    CSSStyleSheet* mSheet;
    bool operator()(const Entry& aEntry)
    { return aEntry.mSheets.Contains(mSheet); }
  };

  ExpirationTracker mExpirationTracker;
  nsTArray<Entry> mEntries;
};

} // namespace mozilla

#endif // mozilla_RuleProcessorCache_h
