#include <unordered_map> 
#include <functional>
#include <iostream>
#include <random> 
#include <cassert>
#include <climits>
#include <queue>

#define HIGH_MASK       0xffffffff00000000
#define LOW_MASK        0xffffffff
#define HALF_WORD_MASK  0xffff

using namespace std;

class Dependence {
public:
  int addr;
  short dir;

  Dependence(int addr, int dir) : addr(addr), dir(dir) {}
};

class TaskDescriptor {
  mt19937 rng;
  uniform_int_distribution<mt19937::result_type> numDepsDist;
  uniform_int_distribution<mt19937::result_type> addrDist;
  uniform_int_distribution<mt19937::result_type> dirDist;

  void initializeRandomDists() {
    addrDist = uniform_int_distribution<mt19937::result_type>(1, 10000);
    numDepsDist = uniform_int_distribution<mt19937::result_type>(0, 15);
    dirDist = uniform_int_distribution<mt19937::result_type>(0, 2);
  }

  void initializeRandomGenerator() {
    rng.seed(random_device()());
  }

  void fillWithRandomDeps() {
    numDeps = numDepsDist(rng);
     
    for (int i = 0; i < numDeps; i++){
      deps.push_back(Dependence(addrDist(rng), dirDist(rng)));
    }
  }

public:
  unsigned long long int swID;
  unsigned long int picosID = -1;
  int numDeps = -1;
  vector<Dependence> deps;

  TaskDescriptor(int swID = -1, string call_site = "?") : swID(swID) {
    //cerr << "[Called by " << call_site << "]: Creating a random TaskDescriptor with swID = " << swID << endl;
    initializeRandomDists();
    initializeRandomGenerator();
    fillWithRandomDeps();
  }

	operator std::string() const { 
		return "Task(swID = " + to_string(swID) + ", picosID = " + to_string(picosID) + ", numDeps = " + to_string(numDeps) + ")";
	}

  int hash() {
    return ((swID & HIGH_MASK) >> 32) ^ (swID & LOW_MASK) ^ picosID;
  }

  TaskDescriptor(unsigned long picosID, unsigned long long swID, int numDeps = -1) : swID(swID), picosID(picosID), numDeps(numDeps) {}

  int operator[](int idx) {
    switch(idx) {
      case 0: return (swID & HIGH_MASK) >> 32;
      case 1: return swID & LOW_MASK;
      case 2: return numDeps;
    }

    int depIdx = (idx - 3) / 3;
    int subDepIdx = (idx - 3) % 3;

    if (depIdx < deps.size()) {
      switch(subDepIdx) {
        case 0: return (deps[depIdx].addr & HIGH_MASK) >> 32;
        case 1: return deps[depIdx].addr & LOW_MASK;
        case 2: return deps[depIdx].dir;
      }
    } else {
      return 0;
    }
  }

  int numEncodingPackets() {
    return 3 + 3 * deps.size();
  }
};

class SWPicosModel {
  queue<TaskDescriptor> submittedTasks;
  queue<TaskDescriptor> readyTasks;
  queue<int> availableIds;
  queue<int> picosIDsToRetire;
  int subCounter = 48;
  int readyWriteCounter = 0;

public:
  unordered_map<int, TaskDescriptor> inFlightTasks;
  queue<int> subQueue;
  queue<int> readyQueue;
  queue<int> retQueue;

  void readFromSubmissionQueue() {
    static vector<int> currentTaskSubPackets;

    if (subCounter > 0) {
      if (!subQueue.empty()) {
        currentTaskSubPackets.push_back(subQueue.front());
        subQueue.pop();
        subCounter--;
      }
    } else {
      subCounter = 48;

      unsigned long int swID = ((unsigned long) currentTaskSubPackets[0] << 32) | currentTaskSubPackets[1];
      int numDeps = currentTaskSubPackets[2] & HALF_WORD_MASK;

      assert(!availableIds.empty());
      int picosID = availableIds.front();
      availableIds.pop();

      cerr << "[PicosModel]: Creating task with (swID, picosID, numDeps) = (" << swID << ", " << picosID << ", " << numDeps << ")" << endl;
      TaskDescriptor newTask = TaskDescriptor(picosID, swID, numDeps);

      for (int i = 0; i < numDeps; i++) {
        unsigned long int addr = ((unsigned long) currentTaskSubPackets[3 + i * 3] << 32) | currentTaskSubPackets[4 + i * 3];
        int dir = currentTaskSubPackets[5 + i * 3];
        newTask.deps.push_back(Dependence(addr, dir));
      }

      submittedTasks.push(newTask);
      currentTaskSubPackets.clear();
      debug_submission_count++;
    }
  }

  void writeToReadyQueue() {
    if (readyWriteCounter > 0) {
      TaskDescriptor & currentTask = readyTasks.front();
      int packet;

      switch(readyWriteCounter) {
        case 4: packet = (currentTask.swID & HIGH_MASK) >> 32;
                break;
        case 3: packet = currentTask.swID & LOW_MASK;
                break;
        case 2: packet = currentTask.picosID;
                break;
        case 1: packet = currentTask.hash();
                break;
        default: assert(false);
      }

      readyWriteCounter--;
      readyQueue.push(packet);

      if (readyWriteCounter == 0) {
        readyTasks.pop();
      }
    } else {
      if (!readyTasks.empty()) {
        readyWriteCounter = 4;
      }
    }
  }

  void readFromRetirementInterface() {
    if (!retQueue.empty()) {
      int picosID = retQueue.front();

      // Send this picosID to a queue to be
      // processed by the retirement engine.
      // 
      // The same ID should only be available
      // again in `availableIds` once that
      // processing has already taken place.
      picosIDsToRetire.push(picosID);
			retQueue.pop();
    }
  }

  void submissionEngineStep() {
    // FIXME
    // The following code makes sure that only one
    // in-flight task may exist at any given moment.
    if (inFlightTasks.empty() && !submittedTasks.empty()) {
      //cerr << "[Picos]: Pushing task to readyTasks" << endl;
      TaskDescriptor & task = submittedTasks.front();
      readyTasks.push(task);
      inFlightTasks[task.picosID] = task;
      submittedTasks.pop();
    }
  }

  // FIXME
  // As of now, this code simply removes the retired
  // task from the map of in-flight tasks, consumes
  // the `picosIDsToRetire` queue and replenishes the
  // availableIds queue.
  void retirementEngineStep() {
    if (!picosIDsToRetire.empty()) {
      int idToRetire = picosIDsToRetire.front();
      cerr << "[SWPicosModel::retirementEngineStep]: idToRetire = " << idToRetire << endl;

      assert(inFlightTasks.count(idToRetire) == 1);
      inFlightTasks.erase(idToRetire);
      availableIds.push(idToRetire);
      picosIDsToRetire.pop();
      debug_retirement_count++;
    }
  }

  SWPicosModel() {}

public:
  int debug_retirement_count = 0;
  int debug_submission_count = 0;

  SWPicosModel(int maxTickets) {
    for (int i = 0; i < maxTickets; i++) {
      availableIds.push(i);
    }
  }

  void step() {
    readFromRetirementInterface();
    retirementEngineStep();
    readFromSubmissionQueue();
    submissionEngineStep();
    writeToReadyQueue();
  }
};

class InstructionHandler {
  SWPicosModel & picosModel;
  queue<int> numPacketsPerSubmission;
  queue<TaskDescriptor> decodedReadyTasks;
  int pendingWorkRequests = 0;
  bool nanosIDWasFetched = false;

  bool readyDecodeStep() {
    static vector<unsigned long int> packetVec;

    if (!picosModel.readyQueue.empty()) {
      assert(packetVec.size() < 4);

      unsigned long int newPacket = picosModel.readyQueue.front();
      picosModel.readyQueue.pop();

      packetVec.push_back(newPacket);

      if (packetVec.size() == 4) {
        unsigned long long swID_high = packetVec[0];
        unsigned long long swID_low = packetVec[1];
        unsigned long long swID = (swID_high << 32) | swID_low;
        unsigned long picosID = packetVec[2];

        assert(packetVec[0] ^ packetVec[1] ^ packetVec[2] == packetVec[3]);
        packetVec.clear();

        TaskDescriptor newReadyTask(picosID, swID);
        decodedReadyTasks.push(newReadyTask);
      }

      return true;
    } else {
      return false;
    }
  }

public:
  InstructionHandler(SWPicosModel &picosModel) : picosModel(picosModel) {}

  void subRequestInst(int numPackets) {
    numPacketsPerSubmission.push(numPackets);
  }

  void submissionInst(int packet) {
    static int numPacketsSentThisRound = 0;

    assert(!numPacketsPerSubmission.empty());

    picosModel.subQueue.push(packet);
    numPacketsSentThisRound++;
    cerr << "[InstructionHandler::submissionInst]: Just sent a packet; numPacketsSentThisRound = " << numPacketsSentThisRound <<  endl;

    while (true) {
      if (!numPacketsPerSubmission.empty() && numPacketsSentThisRound == numPacketsPerSubmission.front()) {
        int remainingZeros = 48 - numPacketsSentThisRound;

        for (int i = 0; i < remainingZeros; i++) {
          picosModel.subQueue.push(0);
        }
        numPacketsSentThisRound = 0;
        numPacketsPerSubmission.pop();
      } else {
        break;
      }
    }
  }

  int workRequestInst(int coreID) {
    return ++pendingWorkRequests;
  }

  unsigned long long int nanosIDFetchingInst() {
    if (decodedReadyTasks.empty() || nanosIDWasFetched) {
      return ULLONG_MAX;
    } else {
      nanosIDWasFetched = true;
      return decodedReadyTasks.front().swID;
    }
  }

  unsigned long int picosIDFetchingInst() {
    if (decodedReadyTasks.empty() || !nanosIDWasFetched) {
      return ULLONG_MAX;
    } else {
      unsigned long int picosID = decodedReadyTasks.front().picosID; 
      decodedReadyTasks.pop();
      nanosIDWasFetched = false;
      pendingWorkRequests--;
      return picosID;
    }
  }

  void retirementInst(int picosID) {
    picosModel.retQueue.push(picosID);
  }

  void step() {
    picosModel.step();
    while (readyDecodeStep()) {}
  }
};
