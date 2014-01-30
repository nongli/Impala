// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_EXEC_EXAMPLE_SCANNER_H
#define IMPALA_EXEC_EXAMPLE_SCANNER_H

#include "exec/hdfs-scanner.h"

namespace impala {

struct HdfsFileDesc;

// This is a sample scanner intended to illustrate how to build an Impala scanner for
// custom file formats. This demonstrates how to interact with our storage abstractions
// and materialize tuples into Impala's in memory row format.
// The example file format that this scanner reads is a trivial binary file format that
// only supports two types:
//   Int32s - which are encoded as little endian
//   Strings - which are encoded with the length as little endian, followed by the byte
//      string. If the length is 0, the column is NULL (empty strings are not possible).
// Records can consist of any number of the two types and records are stored back to back.
// For example, if the schema of the file was <int>, <string>, <int>:
// A valid file for the schema would be:
//  0x0001 0x0004 0xABCD 0x000F
// Corresponding to the data <1, "ABCD", 16>
class ExampleScanner : public HdfsScanner {
 public:
  ExampleScanner(HdfsScanNode* scan_node, RuntimeState* state);

  // The scan node will create these objects as needed. Most of the cleanup should have
  // already been done in Close() so this the dtor is only for very small convenience
  // objects.
  virtual ~ExampleScanner();

  // These are the HdfsScanner superclass APIs that must be implemented.

  // This is called to initialize the scanner. At this point, the requested projection
  // is known and the scanner should initialize any state necessary to parse the file.
  virtual Status Prepare(ScannerContext* context);

  // This is called to process the file end to end. The majority of the logic should
  // happen here. The scanner should read from the ScannerContext until a termination
  // criteria is met.
  virtual Status ProcessSplit();

  // This is called and any expensive resources must be freed. This means any memory
  // that was significant enough to be tracked needs to be freed here.
  virtual void Close();

  // This is called before any Scanner objects are created and is the opportunity to
  // map from physical HDFS blocks into logical processing ranges. In this example, the
  // file is not splittable, meaning we can't process a block that starts in the middle
  // of the file. Consider the case where we only have 1 HDFS file but it is split into
  // two blocks, one starting at byte offset 0 and one at 128MB. IssueInitialRanges()
  // will be called with two splits and this scanner will map that to 1 logical block
  // (by dropping the block that doesn't start at 0 and extending the first block to
  // the length of the file).
  static Status IssueInitialRanges(HdfsScanNode*, const std::vector<HdfsFileDesc*>&);

 private:
  // For each column in the file, collect the type (from the table schema) and either or
  // not the column is materialized. If the query needs this column, the planner will
  // mark the column as being materialized and the scanner must populate the value.
  // e.g. If the schema was "c1 int, c2 string, c3 int, c4 string"
  // and the query was select c1, c3. Then only the 0th and 2nd columns would be marked
  // as materialized.
  struct ColumnDesc {
    // Set to the type of the column, materialized or not.
    PrimitiveType type;
    // Set to the description of slot value. NULL if not materialized.
    SlotDescriptor* desc;
  };
  std::vector<ColumnDesc> cols_;

  // Reads and materializes a single tuple into tuple. The byte stream is assumed to start
  // at the beginning of a complete record.
  // Returns true if a tuple was materialized.
  bool MaterializeRecord(Tuple* tuple, MemPool* pool);
};

}

#endif
