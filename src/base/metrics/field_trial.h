// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// FieldTrial is a class for handling details of statistical experiments
// performed by actual users in the field (i.e., in a shipped or beta product).
// All code is called exclusively on the UI thread currently.
//
// The simplest example is an experiment to see whether one of two options
// produces "better" results across our user population.  In that scenario, UMA
// data is uploaded to aggregate the test results, and this FieldTrial class
// manages the state of each such experiment (state == which option was
// pseudo-randomly selected).
//
// States are typically generated randomly, either based on a one time
// randomization (which will yield the same results, in terms of selecting
// the client for a field trial or not, for every run of the program on a
// given machine), or by a session randomization (generated each time the
// application starts up, but held constant during the duration of the
// process).

//------------------------------------------------------------------------------
// Example:  Suppose we have an experiment involving memory, such as determining
// the impact of some pruning algorithm.
// We assume that we already have a histogram of memory usage, such as:

//   UMA_HISTOGRAM_COUNTS_1M("Memory.RendererTotal", count);

// Somewhere in main thread initialization code, we'd probably define an
// instance of a FieldTrial, with code such as:

// // FieldTrials are reference counted, and persist automagically until
// // process teardown, courtesy of their automatic registration in
// // FieldTrialList.
// // Note: This field trial will run in Chrome instances compiled through
// //       8 July, 2015, and after that all instances will be in "StandardMem".
// scoped_refptr<base::FieldTrial> trial(
//     base::FieldTrialList::FactoryGetFieldTrial(
//         "MemoryExperiment", 1000, "StandardMem",
//          base::FieldTrialList::GetEntropyProviderForOneTimeRandomization());
//
// trial->AppendGroup("HighMem", 20);  // 2% in HighMem group.
// trial->AppendGroup("LowMem", 20);   // 2% in LowMem group.
// // Take action depending of which group we randomly land in.
// if (trial->group_name() == "HighMem")
//   SetPruningAlgorithm(kType1);  // Sample setting of browser state.
// else if (trial->group_name() == "LowMem")
//   SetPruningAlgorithm(kType2);  // Sample alternate setting.

//------------------------------------------------------------------------------

#ifndef BASE_METRICS_FIELD_TRIAL_H_
#define BASE_METRICS_FIELD_TRIAL_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/atomicops.h"
#include "base/base_export.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/ref_counted.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/metrics/persistent_memory_allocator.h"
#include "base/pickle.h"
#include "base/strings/string_piece.h"
#include "base/synchronization/lock.h"
#include "base/types/pass_key.h"
#include "build/build_config.h"

namespace base {

namespace test {
class ScopedFeatureList;
}  // namespace test

class FieldTrialList;
struct LaunchOptions;

class BASE_EXPORT FieldTrial : public RefCounted<FieldTrial> {
 public:
  typedef int Probability;  // Probability type for being selected in a trial.

  // EntropyProvider is an interface for providing entropy for one-time
  // randomized (persistent) field trials.
  class BASE_EXPORT EntropyProvider {
   public:
    virtual ~EntropyProvider();

    // Returns a double in the range of [0, 1) to be used for the dice roll for
    // the specified field trial. If |randomization_seed| is not 0, it will be
    // used in preference to |trial_name| for generating the entropy by entropy
    // providers that support it. A given instance should always return the same
    // value given the same input |trial_name| and |randomization_seed| values.
    virtual double GetEntropyForTrial(StringPiece trial_name,
                                      uint32_t randomization_seed) const = 0;
  };

  // Separate type from FieldTrial::PickleState so that it can use StringPieces.
  struct State {
    StringPiece trial_name;
    StringPiece group_name;
    bool activated = false;
  };

  // A pair representing a Field Trial and its selected group.
  struct ActiveGroup {
    std::string trial_name;
    std::string group_name;
  };

  // A triplet representing a FieldTrial, its selected group and whether it's
  // active. String members are pointers to the underlying strings owned by the
  // FieldTrial object. Does not use StringPiece to avoid conversions back to
  // std::string.
  struct BASE_EXPORT PickleState {
    raw_ptr<const std::string, DanglingUntriaged> trial_name = nullptr;
    raw_ptr<const std::string, DanglingUntriaged> group_name = nullptr;
    bool activated = false;

    PickleState();
    PickleState(const PickleState& other);
    ~PickleState();
  };

  // We create one FieldTrialEntry per field trial in shared memory, via
  // AddToAllocatorWhileLocked. The FieldTrialEntry is followed by a
  // base::Pickle object that we unpickle and read from.
  struct BASE_EXPORT FieldTrialEntry {
    // SHA1(FieldTrialEntry): Increment this if structure changes!
    static constexpr uint32_t kPersistentTypeId = 0xABA17E13 + 3;

    // Expected size for 32/64-bit check.
    static constexpr size_t kExpectedInstanceSize = 16;

    // Whether or not this field trial is activated. This is really just a
    // boolean but using a 32 bit value for portability reasons. It should be
    // accessed via NoBarrier_Load()/NoBarrier_Store() to prevent the compiler
    // from doing unexpected optimizations because it thinks that only one
    // thread is accessing the memory location.
    subtle::Atomic32 activated;

    // On e.g. x86, alignof(uint64_t) is 4.  Ensure consistent size and
    // alignment of `pickle_size` across platforms.
    uint32_t padding;

    // Size of the pickled structure, NOT the total size of this entry.
    uint64_t pickle_size;

    // Calling this is only valid when the entry is initialized. That is, it
    // resides in shared memory and has a pickle containing the trial name and
    // group name following it.
    bool GetTrialAndGroupName(StringPiece* trial_name,
                              StringPiece* group_name) const;

    // Calling this is only valid when the entry is initialized as well. Reads
    // the parameters following the trial and group name and stores them as
    // key-value mappings in |params|.
    bool GetParams(std::map<std::string, std::string>* params) const;

   private:
    // Returns an iterator over the data containing names and params.
    PickleIterator GetPickleIterator() const;

    // Takes the iterator and writes out the first two items into |trial_name|
    // and |group_name|.
    bool ReadStringPair(PickleIterator* iter,
                        StringPiece* trial_name,
                        StringPiece* group_name) const;
  };

  typedef std::vector<ActiveGroup> ActiveGroups;

  // A return value to indicate that a given instance has not yet had a group
  // assignment (and hence is not yet participating in the trial).
  static const int kNotFinalized;

  FieldTrial(const FieldTrial&) = delete;
  FieldTrial& operator=(const FieldTrial&) = delete;

  // Establishes the name and probability of the next group in this trial.
  // Sometimes, based on construction randomization, this call may cause the
  // provided group to be *THE* group selected for use in this instance.
  // AppendGroup can be called after calls to group() but it should be avoided
  // if possible. Doing so may be confusing since it won't change the group
  // selection.
  void AppendGroup(const std::string& name, Probability group_probability);

  // Return the name of the FieldTrial (excluding the group name).
  const std::string& trial_name() const { return trial_name_; }

  // Finalizes the group assignment and notifies any/all observers. This is a
  // no-op if the trial is already active. Note this will force an instance to
  // participate, and make it illegal to attempt to probabilistically add any
  // other groups to the trial.
  void Activate();

  // If the group's name is empty, a string version containing the group number
  // is used as the group name. This causes a winner to be chosen if none was.
  const std::string& group_name();

  // Finalizes the group choice and returns the chosen group, but does not mark
  // the trial as active - so its state will not be reported until group_name()
  // or similar is called.
  const std::string& GetGroupNameWithoutActivation();

  // Set the field trial as forced, meaning that it was setup earlier than
  // the hard coded registration of the field trial to override it.
  // This allows the code that was hard coded to register the field trial to
  // still succeed even though the field trial has already been registered.
  // This must be called after appending all the groups, since we will make
  // the group choice here. Note that this is a NOOP for already forced trials.
  // And, as the rest of the FieldTrial code, this is not thread safe and must
  // be done from the UI thread.
  void SetForced();

  // Supports benchmarking by causing field trials' default groups to be chosen.
  static void EnableBenchmarking();

  // Creates a FieldTrial object with the specified parameters, to be used for
  // simulation of group assignment without actually affecting global field
  // trial state in the running process. Group assignment will be done based on
  // |entropy_value|, which must have a range of [0, 1).
  //
  // Note: Using this function will not register the field trial globally in the
  // running process - for that, use FieldTrialList::FactoryGetFieldTrial().
  //
  // The ownership of the returned FieldTrial is transfered to the caller which
  // is responsible for deref'ing it (e.g. by using scoped_refptr<FieldTrial>).
  static FieldTrial* CreateSimulatedFieldTrial(StringPiece trial_name,
                                               Probability total_probability,
                                               StringPiece default_group_name,
                                               double entropy_value);

 private:
  // Allow tests to access our innards for testing purposes.
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, Registration);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, AbsoluteProbabilities);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, RemainingProbability);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, FiftyFiftyProbability);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, MiddleProbabilities);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, OneWinner);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, DisableProbability);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, ActiveGroups);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, AllGroups);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, ActiveGroupsNotFinalized);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, Save);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, SaveAll);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, DuplicateRestore);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, SetForcedTurnFeatureOff);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, SetForcedTurnFeatureOn);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, SetForcedChangeDefault_Default);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, SetForcedChangeDefault_NonDefault);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, ObserveReentrancy);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, FloatBoundariesGiveEqualGroupSizes);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialTest, DoesNotSurpassTotalProbability);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest,
                           DoNotAddSimulatedFieldTrialsToAllocator);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest, ClearParamsFromSharedMemory);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest,
                           TestGetRandomizedFieldTrialCount);

  friend class base::FieldTrialList;

  friend class RefCounted<FieldTrial>;

  using FieldTrialRef = PersistentMemoryAllocator::Reference;

  // This is the group number of the 'default' group when a choice wasn't forced
  // by a call to FieldTrialList::CreateFieldTrial. It is kept private so that
  // consumers don't use it by mistake in cases where the group was forced.
  static const int kDefaultGroupNumber;

  // Creates a field trial with the specified parameters. Group assignment will
  // be done based on |entropy_value|, which must have a range of [0, 1).
  FieldTrial(StringPiece trial_name,
             Probability total_probability,
             StringPiece default_group_name,
             double entropy_value);

  virtual ~FieldTrial();

  // Return the default group name of the FieldTrial.
  const std::string& default_group_name() const { return default_group_name_; }

  // Marks this trial as having been registered with the FieldTrialList. Must be
  // called no more than once and before any |group()| calls have occurred.
  void SetTrialRegistered();

  // Sets the chosen group name and number.
  void SetGroupChoice(const std::string& group_name, int number);

  // Ensures that a group is chosen, if it hasn't yet been. The field trial
  // might yet be disabled, so this call will *not* notify observers of the
  // status.
  void FinalizeGroupChoice();

  // Implements FinalizeGroupChoice() with the added flexibility of being
  // deadlock-free if |is_locked| is true and the caller is holding a lock.
  void FinalizeGroupChoiceImpl(bool is_locked);

  // Returns the trial name and selected group name for this field trial via
  // the output parameter |active_group|, but only if the group has already
  // been chosen and has been externally observed via |group()| and the trial
  // has not been disabled. In that case, true is returned and |active_group|
  // is filled in; otherwise, the result is false and |active_group| is left
  // untouched.
  bool GetActiveGroup(ActiveGroup* active_group) const;

  // Returns the trial name and selected group name for this field trial via
  // the output parameter |field_trial_state| for all the studies.
  void GetStateWhileLocked(PickleState* field_trial_state);

  // Returns the group_name. A winner need not have been chosen.
  const std::string& group_name_internal() const { return group_name_; }

  // The name of the field trial, as can be found via the FieldTrialList.
  const std::string trial_name_;

  // The maximum sum of all probabilities supplied, which corresponds to 100%.
  // This is the scaling factor used to adjust supplied probabilities.
  const Probability divisor_;

  // The name of the default group.
  const std::string default_group_name_;

  // The randomly selected probability that is used to select a group (or have
  // the instance not participate).  It is the product of divisor_ and a random
  // number between [0, 1).
  Probability random_;

  // Sum of the probabilities of all appended groups.
  Probability accumulated_group_probability_;

  // The number that will be returned by the next AppendGroup() call.
  int next_group_number_;

  // The pseudo-randomly assigned group number.
  // This is kNotFinalized if no group has been assigned.
  int group_;

  // A textual name for the randomly selected group. Valid after |group()|
  // has been called.
  std::string group_name_;

  // When forced_ is true, we return the chosen group from AppendGroup when
  // appropriate.
  bool forced_;

  // Specifies whether the group choice has been reported to observers.
  bool group_reported_;

  // Whether this trial is registered with the global FieldTrialList and thus
  // should notify it when its group is queried.
  bool trial_registered_;

  // Reference to related field trial struct and data in shared memory.
  FieldTrialRef ref_;

  // Denotes whether benchmarking is enabled. In this case, field trials all
  // revert to the default group.
  static bool enable_benchmarking_;
};

//------------------------------------------------------------------------------
// Class with a list of all active field trials.  A trial is active if it has
// been registered, which includes evaluating its state based on its probaility.
// Only one instance of this class exists and outside of testing, will live for
// the entire life time of the process.
class BASE_EXPORT FieldTrialList {
 public:
  using FieldTrialAllocator = PersistentMemoryAllocator;

  // Type for function pointer passed to |AllParamsToString| used to escape
  // special characters from |input|.
  typedef std::string (*EscapeDataFunc)(const std::string& input);

  // Observer is notified when a FieldTrial's group is selected.
  class BASE_EXPORT Observer {
   public:
    // Notify observers when FieldTrials's group is selected.
    virtual void OnFieldTrialGroupFinalized(const std::string& trial_name,
                                            const std::string& group_name) = 0;

   protected:
    virtual ~Observer();
  };

  // This singleton holds the global list of registered FieldTrials.
  //
  // To support one-time randomized field trials, specify a non-null
  // |entropy_provider| which should be a source of uniformly distributed
  // entropy values. If one time randomization is not desired, pass in null for
  // |entropy_provider|.
  explicit FieldTrialList(
      std::unique_ptr<const FieldTrial::EntropyProvider> entropy_provider);
  FieldTrialList(const FieldTrialList&) = delete;
  FieldTrialList& operator=(const FieldTrialList&) = delete;

  // Destructor Release()'s references to all registered FieldTrial instances.
  ~FieldTrialList();

  // Gets a FieldTrial instance from the factory.
  //
  // |trial_name| (a) is used to register the instance with the FieldTrialList
  // class and (b) can be used to find the trial (only one trial can be present
  // for each name). |default_group_name| is the name of the group that is
  // chosen if none of the subsequent appended groups are chosen. Note that the
  // default group is also chosen whenever |enable_benchmarking_| is true.
  //
  // Group probabilities that are later supplied must sum to less than or equal
  // to the |total_probability|.
  //
  // The |entropy_provider| is used for randomizing group selection. The
  // |randomization_seed| will be passed to the EntropyProvider in addition
  // to the trial name, and it's handling is defined by the EntropyProvider.
  // * SessionEntropyProvider requires it to be 0 by DCHECK.
  // * SHA1 and NormalizedMurmurHash providers will use a non-zero value as a
  //   salt _instead_ of using the trial name.
  //
  // This static method can be used to get a startup-randomized FieldTrial or a
  // previously created forced FieldTrial.
  static FieldTrial* FactoryGetFieldTrial(
      StringPiece trial_name,
      FieldTrial::Probability total_probability,
      StringPiece default_group_name,
      const FieldTrial::EntropyProvider& entropy_provider,
      uint32_t randomization_seed = 0);

  // The Find() method can be used to test to see if a named trial was already
  // registered, or to retrieve a pointer to it from the global map.
  static FieldTrial* Find(StringPiece trial_name);

  // Returns the group number chosen for the named trial, or
  // FieldTrial::kNotFinalized if the trial does not exist.
  static int FindValue(StringPiece trial_name);

  // Returns the group name chosen for the named trial, or the empty string if
  // the trial does not exist. The first call of this function on a given field
  // trial will mark it as active, so that its state will be reported with usage
  // metrics, crashes, etc.
  // Note: Direct use of this function and related FieldTrial functions is
  // generally discouraged - instead please use base::Feature when possible.
  static std::string FindFullName(StringPiece trial_name);

  // Returns true if the named trial has been registered.
  static bool TrialExists(StringPiece trial_name);

  // Returns true if the named trial exists and has been activated.
  static bool IsTrialActive(StringPiece trial_name);

  // Creates a persistent representation of active FieldTrial instances for
  // resurrection in another process. This allows randomization to be done in
  // one process, and secondary processes can be synchronized on the result.
  // The resulting string contains the name and group name pairs of all
  // registered FieldTrials for which the group has been chosen and externally
  // observed (via |group()|) and which have not been disabled, with "/" used
  // to separate all names and to terminate the string. This string is parsed
  // by |CreateTrialsFromString()|.
  static void StatesToString(std::string* output);

  // Creates a persistent representation of all FieldTrial instances for
  // resurrection in another process. This allows randomization to be done in
  // one process, and secondary processes can be synchronized on the result.
  // The resulting string contains the name and group name pairs of all
  // registered FieldTrials,
  // with "/" used to separate all names and to terminate the string. All
  // activated trials have their name prefixed with "*". This string is parsed
  // by |CreateTrialsFromString()|.
  static void AllStatesToString(std::string* output);

  // Creates a persistent representation of all FieldTrial params for
  // resurrection in another process. The returned string contains the trial
  // name and group name pairs of all registered FieldTrials. The pair is
  // followed by ':' separator and list of param name and values separated by
  // '/'. It also takes |encode_data_func| function pointer for encodeing
  // special characters. This string is parsed by
  // |AssociateParamsFromString()|.
  static std::string AllParamsToString(EscapeDataFunc encode_data_func);

  // Fills in the supplied vector |active_groups| (which must be empty when
  // called) with a snapshot of all registered FieldTrials for which the group
  // has been chosen and externally observed (via |group()|) and which have
  // not been disabled.
  static void GetActiveFieldTrialGroups(
      FieldTrial::ActiveGroups* active_groups);

  // Returns the field trials that are marked active in |trials_string|.
  static void GetActiveFieldTrialGroupsFromString(
      const std::string& trials_string,
      FieldTrial::ActiveGroups* active_groups);

  // Returns the field trials that were active when the process was
  // created. Either parses the field trial string or the shared memory
  // holding field trial information.
  // Must be called only after a call to CreateTrialsFromCommandLine().
  static void GetInitiallyActiveFieldTrials(
      const CommandLine& command_line,
      FieldTrial::ActiveGroups* active_groups);

  // Use a state string (re: StatesToString()) to augment the current list of
  // field trials to include the supplied trials, and using a 100% probability
  // for each trial, force them to have the same group string. This is commonly
  // used in a non-browser process, to carry randomly selected state in a
  // browser process into this non-browser process, but could also be invoked
  // through a command line argument to the browser process. Created field
  // trials will be marked "used" for the purposes of active trial reporting
  // if they are prefixed with |kActivationMarker|.
  static bool CreateTrialsFromString(const std::string& trials_string);

  // Achieves the same thing as CreateTrialsFromString, except wraps the logic
  // by taking in the trials from the command line, either via shared memory
  // handle or command line argument.
  // On non-Mac POSIX platforms, we simply get the trials from opening |fd_key|
  // if using shared memory. The argument is needed here since //base can't
  // depend on //content. |fd_key| is unused on other platforms.
  // On other platforms, we expect the |cmd_line| switch for kFieldTrialHandle
  // to contain the shared memory handle that contains the field trial
  // allocator.
  static void CreateTrialsFromCommandLine(const CommandLine& cmd_line,
                                          uint32_t fd_key);

  // Creates base::Feature overrides from the command line by first trying to
  // use shared memory and then falling back to the command line if it fails.
  static void CreateFeaturesFromCommandLine(const CommandLine& command_line,
                                            FeatureList* feature_list);

#if !BUILDFLAG(IS_IOS)
  // Populates |command_line| and |launch_options| with the handles and command
  // line arguments necessary for a child process to inherit the shared-memory
  // object containing the FieldTrial configuration.
  static void PopulateLaunchOptionsWithFieldTrialState(
      CommandLine* command_line,
      LaunchOptions* launch_options);
#endif  // !BUILDFLAG(IS_IOS)

#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_APPLE) && !BUILDFLAG(IS_NACL)
  // On POSIX, we also need to explicitly pass down this file descriptor that
  // should be shared with the child process. Returns -1 if it was not
  // initialized properly. The current process remains the onwer of the passed
  // descriptor.
  static int GetFieldTrialDescriptor();
#endif  // BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_APPLE) && !BUILDFLAG(IS_NACL)

  static ReadOnlySharedMemoryRegion DuplicateFieldTrialSharedMemoryForTesting();

  // Create a FieldTrial with the given |name| and using 100% probability for
  // the FieldTrial, force FieldTrial to have the same group string as
  // |group_name|. This is commonly used in a non-browser process, to carry
  // randomly selected state in a browser process into this non-browser process.
  // It returns NULL if there is a FieldTrial that is already registered with
  // the same |name| but has different finalized group string (|group_name|).
  static FieldTrial* CreateFieldTrial(StringPiece name, StringPiece group_name);

  // Add an observer to be notified when a field trial is irrevocably committed
  // to being part of some specific field_group (and hence the group_name is
  // also finalized for that field_trial). Returns false and does nothing if
  // there is no FieldTrialList singleton. The observer can be notified on any
  // sequence; it must be thread-safe.
  static bool AddObserver(Observer* observer);

  // Remove an observer. This cannot be invoked concurrently with
  // FieldTrial::group() (typically, this means that no other thread should be
  // running when this is invoked).
  static void RemoveObserver(Observer* observer);

  // Grabs the lock if necessary and adds the field trial to the allocator. This
  // should only be called from FinalizeGroupChoice().
  static void OnGroupFinalized(bool is_locked, FieldTrial* field_trial);

  // Notify all observers that a group has been finalized for |field_trial|.
  static void NotifyFieldTrialGroupSelection(FieldTrial* field_trial);

  // Return the number of active field trials.
  static size_t GetFieldTrialCount();

  // Return the number of active field trials registered as randomized trials.
  // Trials created using the CreateFieldTrial() do not count towards this
  // total.
  static size_t GetRandomizedFieldTrialCount();

  // Gets the parameters for |field_trial| from shared memory and stores them in
  // |params|. This is only exposed for use by FieldTrialParamAssociator and
  // shouldn't be used by anything else.
  static bool GetParamsFromSharedMemory(
      FieldTrial* field_trial,
      std::map<std::string, std::string>* params);

  // Clears all the params in the allocator.
  static void ClearParamsFromSharedMemoryForTesting();

  // Dumps field trial state to an allocator so that it can be analyzed after a
  // crash.
  static void DumpAllFieldTrialsToPersistentAllocator(
      PersistentMemoryAllocator* allocator);

  // Retrieves field trial state from an allocator so that it can be analyzed
  // after a crash. The pointers in the returned vector are into the persistent
  // memory segment and so are only valid as long as the allocator is valid.
  static std::vector<const FieldTrial::FieldTrialEntry*>
  GetAllFieldTrialsFromPersistentAllocator(
      PersistentMemoryAllocator const& allocator);

  // Returns the EntropyProvider for one-time randomization.
  // Crashes if one-time randomization is not enabled.
  static const FieldTrial::EntropyProvider&
  GetEntropyProviderForOneTimeRandomization();

  // Returns an entropy-provider that can be used for session randomized trials.
  static const FieldTrial::EntropyProvider&
  GetEntropyProviderForSessionRandomization();

  // Returns a pointer to the global instance. This is exposed so that it can
  // be used in a DCHECK in FeatureList and ScopedFeatureList test-only logic
  // and is not intended to be used widely beyond those cases.
  static FieldTrialList* GetInstance();

  // For testing, sets the global instance to null and returns the previous one.
  static FieldTrialList* BackupInstanceForTesting();

  // For testing, sets the global instance to |instance|.
  static void RestoreInstanceForTesting(FieldTrialList* instance);

  // Creates a list of FieldTrial::State for all FieldTrial instances.
  // StringPiece members are bound to the lifetime of the corresponding
  // FieldTrial.
  static std::vector<FieldTrial::State> GetAllFieldTrialStates(
      PassKey<test::ScopedFeatureList>);

  // Create FieldTrials from a list of FieldTrial::State. This method is only
  // available to ScopedFeatureList for testing. The most typical usescase is:
  // (1) AllStatesToFieldTrialStates(&field_trials);
  // (2) backup_ = BackupInstanceForTesting();
  //     // field_trials depends on backup_'s lifetype.
  // (3) field_trial_list_ = new FieldTrialList();
  // (4) CreateTrialsFromFieldTrialStates(field_trials);
  //     // Copy backup_'s fieldtrials to the new field_trial_list_ while
  //     // backup_ is alive.
  // For resurrestion in another process, need to use AllStatesToString and
  // CreateFieldTrialsFromString.
  static bool CreateTrialsFromFieldTrialStates(
      PassKey<test::ScopedFeatureList>,
      const std::vector<FieldTrial::State>& entries);

 private:
  // Allow tests to access our innards for testing purposes.
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest, InstantiateAllocator);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest, AddTrialsToAllocator);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest,
                           DoNotAddSimulatedFieldTrialsToAllocator);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest, AssociateFieldTrialParams);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest, ClearParamsFromSharedMemory);
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest,
                           SerializeSharedMemoryRegionMetadata);
  friend int SerializeSharedMemoryRegionMetadata();
  FRIEND_TEST_ALL_PREFIXES(FieldTrialListTest, CheckReadOnlySharedMemoryRegion);

#if !BUILDFLAG(IS_NACL) && !BUILDFLAG(IS_IOS)
  // Serialization is used to pass information about the shared memory handle
  // to child processes. This is achieved by passing a stringified reference to
  // the relevant OS resources to the child process.
  //
  // Serialization populates |launch_options| with the relevant OS handles to
  // transfer or copy to the child process and returns serialized information
  // to be passed to the kFieldTrialHandle command-line switch.
  // Note: On non-Mac POSIX platforms, it is necessary to pass down the file
  // descriptor for the shared memory separately. It can be accessed via the
  // GetFieldTrialDescriptor() API.
  static std::string SerializeSharedMemoryRegionMetadata(
      const ReadOnlySharedMemoryRegion& shm,
      LaunchOptions* launch_options);

  // Deserialization instantiates the shared memory region for FieldTrials from
  // the serialized information contained in |switch_value|. Returns an invalid
  // ReadOnlySharedMemoryRegion on failure.
  // |fd| is used on non-Mac POSIX platforms to instantiate the shared memory
  // region via a file descriptor.
  static ReadOnlySharedMemoryRegion DeserializeSharedMemoryRegionMetadata(
      const std::string& switch_value,
      int fd);

  // Takes in |handle_switch| from the command line which represents the shared
  // memory handle for field trials, parses it, and creates the field trials.
  // Returns true on success, false on failure.
  // |switch_value| also contains the serialized GUID.
  // |fd_key| is used on non-Mac POSIX platforms as the file descriptor passed
  // down to the child process for the shared memory region.
  static bool CreateTrialsFromSwitchValue(const std::string& switch_value,
                                          uint32_t fd_key);
#endif  // !BUILDFLAG(IS_NACL) && !BUILDFLAG(IS_IOS)

  // Takes an unmapped ReadOnlySharedMemoryRegion, maps it with the correct size
  // and creates field trials via CreateTrialsFromSharedMemoryMapping(). Returns
  // true if successful and false otherwise.
  static bool CreateTrialsFromSharedMemoryRegion(
      const ReadOnlySharedMemoryRegion& shm_region);

  // Expects a mapped piece of shared memory |shm_mapping| that was created from
  // the browser process's field_trial_allocator and shared via the command
  // line. This function recreates the allocator, iterates through all the field
  // trials in it, and creates them via CreateFieldTrial(). Returns true if
  // successful and false otherwise.
  static bool CreateTrialsFromSharedMemoryMapping(
      ReadOnlySharedMemoryMapping shm_mapping);

  // Instantiate the field trial allocator, add all existing field trials to it,
  // and duplicates its handle to a read-only handle, which gets stored in
  // |readonly_allocator_handle|.
  static void InstantiateFieldTrialAllocatorIfNeeded();

  // Adds the field trial to the allocator. Caller must hold a lock before
  // calling this.
  static void AddToAllocatorWhileLocked(PersistentMemoryAllocator* allocator,
                                        FieldTrial* field_trial);

  // Activate the corresponding field trial entry struct in shared memory.
  static void ActivateFieldTrialEntryWhileLocked(FieldTrial* field_trial);

  // A map from FieldTrial names to the actual instances.
  typedef std::map<std::string, FieldTrial*, std::less<>> RegistrationMap;

  // Helper function should be called only while holding lock_.
  FieldTrial* PreLockedFind(StringPiece name) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Register() stores a pointer to the given trial in a global map.
  // This method also AddRef's the indicated trial.
  // This should always be called after creating a new FieldTrial instance.
  // If the caller wants to select the instance's group randomly,
  // |is_randomized_trial| should be true to count the number of randomized
  // trials correctly. Otherwise, false.
  static void Register(FieldTrial* trial, bool is_randomized_trial);

  // Returns all the registered trials.
  static RegistrationMap GetRegisteredTrials();

  // Create field trials from a list of FieldTrial::State.
  // CreateTrialsFromString() and CreateTrialsFromFieldTrialStates() use this
  // method internally.
  static bool CreateTrialsFromFieldTrialStatesInternal(
      const std::vector<FieldTrial::State>& entries);

  static FieldTrialList* global_;  // The singleton of this class.

  // This will tell us if there is an attempt to register a field
  // trial or check if one-time randomization is enabled without
  // creating the FieldTrialList. This is not an error, unless a
  // FieldTrialList is created after that.
  static bool used_without_global_;

  // Lock for access to |registered_|, |observers_|,
  // |count_of_manually_created_field_trials_|.
  Lock lock_;
  RegistrationMap registered_ GUARDED_BY(lock_);

  // Counts the number of field trials whose groups are selected randomly.
  size_t num_registered_randomized_trials_ GUARDED_BY(lock_) = 0;

  // Entropy provider to be used for one-time randomized field trials. If NULL,
  // one-time randomization is not supported.
  std::unique_ptr<const FieldTrial::EntropyProvider> entropy_provider_;

  // List of observers to be notified when a group is selected for a FieldTrial.
  std::vector<Observer*> observers_ GUARDED_BY(lock_);

  // Counts the ongoing calls to
  // FieldTrialList::NotifyFieldTrialGroupSelection(). Used to ensure that
  // RemoveObserver() isn't called while notifying observers.
  std::atomic_int num_ongoing_notify_field_trial_group_selection_calls_{0};

  // Allocator in shared memory containing field trial data. Used in both
  // browser and child processes, but readonly in the child.
  // In the future, we may want to move this to a more generic place if we want
  // to start passing more data other than field trials.
  std::unique_ptr<FieldTrialAllocator> field_trial_allocator_;

  // Readonly copy of the region to the allocator. Needs to be a member variable
  // because it's needed from multiple methods.
  ReadOnlySharedMemoryRegion readonly_allocator_region_;

  // Tracks whether CreateTrialsFromCommandLine() has been called.
  bool create_trials_from_command_line_called_ = false;
};

}  // namespace base

#endif  // BASE_METRICS_FIELD_TRIAL_H_
