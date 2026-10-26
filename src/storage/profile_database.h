#pragma once

namespace kobalt::storage {
// The profile database stays open on the writer thread while Kobalt runs. See
// the profile connection in annotation_database.cpp.

// Waits for waiting writes, then folds the log into the main file, returns it
// to a rollback journal and closes it. With `keepClosed`, every operation
// fails until reopenProfile(), for example while USB storage is exported or
// the device powers off.
void closeProfile(bool keepClosed = false);
void reopenProfile();
// Syncs recent commits and folds them into the main file, for example before
// sleep.
void checkpointProfile();
// True when routine commits are not yet on storage and the last sync is a
// minute old or more. Standby then runs a checkpoint first. After a failed
// checkpoint it stays false for five minutes.
bool profileCheckpointDue();
// True while routine commits wait for a sync.
bool profileHasUnsyncedWrites();
// Starts a checkpoint on the writer thread without waiting for it.
void requestProfileCheckpoint();
bool profileCheckpointPending();
// Commits that this thread starts while it exists do not sync storage. A
// later synced commit or a checkpoint syncs them. Use only for records that
// are written again soon, such as the reading position.
class RoutineWrites {
public:
    RoutineWrites();
    ~RoutineWrites();
    RoutineWrites(const RoutineWrites &) = delete;
    RoutineWrites &operator=(const RoutineWrites &) = delete;

private:
    bool previous;
};
}
