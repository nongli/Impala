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

#include "exec/example-scanner.h"

#include "exec/hdfs-scan-node.h"
#include "exec/scanner-context.inline.h"
#include "exprs/expr.h"
#include "runtime/descriptors.h"
#include "runtime/runtime-state.h"
#include "runtime/mem-pool.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"
#include "runtime/tuple.h"
#include "runtime/string-value.h"
#include "util/debug-util.h"
#include "util/runtime-profile.h"

using namespace std;
using namespace boost;
using namespace boost::algorithm;
using namespace impala;

Status ExampleScanner::IssueInitialRanges(HdfsScanNode* scan_node,
    const vector<HdfsFileDesc*>& files) {
  vector<DiskIoMgr::ScanRange*> ranges_to_read;

  // A split is a subset of the file and has *no* mapping to the contents in the
  // file. Splittable files are files that allow readers to make sense of the
  // content starting from any point in the file. Non-splittable files must be
  // read from the beginning.
  // The files are typically split at HDFS block boundaries but the scanner can
  // not assume that is the case.
  for (int i = 0; i < files.size(); ++i) {
    for (int j = 0; j < files[i]->splits.size(); ++j) {
      DiskIoMgr::ScanRange* split = files[i]->splits[j];
      if (split->offset() != 0) {
        // Since the file is not splittable, skip splits that don't start at the
        // beginning. We'll just report an error so that the user knows these files
        // are probably bigger than were designed for. The split that starts at the
        // beginning will be responsible for these bytes.
        stringstream ss;
        ss << "File: " << files[i]->filename << " is not intended to be splittable.";
        scan_node->runtime_state()->LogError(ss.str());
        // We need to tell the scan node that this block is "done" since it tracks
        // progress using splits.
        scan_node->RangeComplete(
            THdfsFileFormat::EXAMPLE_SCANNER_FILE, THdfsCompression::NONE);
        continue;
      }

      // We found a split that starts at the beginning, we need to process the entire
      // file.
      ScanRangeMetadata* metadata =
          reinterpret_cast<ScanRangeMetadata*>(files[i]->splits[0]->meta_data());
      DiskIoMgr::ScanRange* footer_range = scan_node->AllocateScanRange(
          files[i]->filename.c_str(), files[i]->file_length, 0,
          metadata->partition_id, files[i]->splits[0]->disk_id());
      ranges_to_read.push_back(footer_range);
    }
  }
  // We've collected the logical ranges. Issue them to be read. Impala has an
  // asynchronous HDFS reading abstraction. This call is non-blocking and simply
  // schedules the ranges to be read at some point. This will handle interleaving
  // IO and CPU, resource management, back pressure, etc.
  RETURN_IF_ERROR(scan_node->AddDiskIoRanges(ranges_to_read));
  return Status::OK;
}

ExampleScanner::ExampleScanner(HdfsScanNode* scan_node, RuntimeState* state)
    : HdfsScanner(scan_node, state) {
}

ExampleScanner::~ExampleScanner() {
  // Usually all the cleanup should be in Close()
}

Status ExampleScanner::Prepare(ScannerContext* context) {
  // First call the super class Prepare() which initializes a lot of state.
  RETURN_IF_ERROR(HdfsScanner::Prepare(context));

  // Get the columns that are exepected to be in this file.
  const HdfsTableDescriptor* table_desc = scan_node_->hdfs_table();
  const TupleDescriptor* tuple_desc = scan_node_->tuple_desc();
  const vector<SlotDescriptor*>& slots = tuple_desc->slots();

  // Loop through each column in the table. This includes non-materialized
  // cols and partitioning columns
  for (int i = 0; i < slots.size(); ++i) {
    if (table_desc->IsClusteringCol(slots[i])) continue;
    // This is a partition col, there is no data that is in the file for this column,
    // skip it.
    if (slots[i]->type() != TYPE_STRING && slots[i]->type() != TYPE_INT) {
      return Status("This file is only expecting STRINGs and INTs.");
    }
    ColumnDesc desc;
    desc.type = slots[i]->type();
    desc.desc = slots[i]->is_materialized() ? slots[i] : NULL;
    cols_.push_back(desc);
  }
  return Status::OK;
}

inline bool ExampleScanner::MaterializeRecord(Tuple* tuple, MemPool* pool) {
  if (stream_->eosr()) return false; // End of file.

  // This needs to be called before populating any of the slots in the tuple. This
  // initializes the null bits in the tuple as well as any partition key values.
  InitTuple(template_tuple_, tuple);

  // Loop through each column in the file.
  for (int i = 0; i < cols_.size(); ++i) {
    SlotDescriptor* desc = cols_[i].desc;
    switch (cols_[i].type) {
      case TYPE_STRING: {
        // Read little endian 4 byte length followed by bytes
        uint8_t* len_ptr = NULL;
        uint8_t* str = NULL;
        if (!stream_->ReadBytes(sizeof(int32_t), &len_ptr, &parse_status_)) goto end;
        int32_t len = *reinterpret_cast<int32_t*>(len_ptr);
        if (!stream_->ReadBytes(len, &str, &parse_status_)) goto end;
        if (desc != NULL) {
          // We've now read the string data and need to populate it.
          StringValue* slot =
              reinterpret_cast<StringValue*>(tuple->GetSlot(desc->tuple_offset()));
          if (len == 0) {
            // Treat empty strings as NULLs. Note that this does not have to be the case
            // but this illustrates how to populate NULL slots.
            tuple->SetNull(desc->null_indicator_offset());
          } else {
            slot->len = len;
            // For string data, we have the option of having the data being in the
            // stream_ object, or copying it out into a separate memory pool. The first
            // approach has no copies but uses more memory (the stream_ contains other
            // bytes that the query does not need). The second generates a compact
            // representation. The planner controls this so the scanner needs to
            // support both cases.
            if (stream_->compact_data()) {
              slot->ptr = reinterpret_cast<char*>(pool->Allocate(len));
              memcpy(slot->ptr, str, len);
            } else {
              slot->ptr = reinterpret_cast<char*>(str);
            }
          }
        }
        break;
      }

      case TYPE_INT: {
        uint8_t* bytes;
        if (!stream_->ReadBytes(sizeof(int32_t), &bytes, &parse_status_)) goto end;
        if (desc != NULL) {
          // Write the result to the tuple.
          int32_t* slot =
              reinterpret_cast<int32_t*>(tuple->GetSlot(desc->tuple_offset()));
          memcpy(slot, bytes, sizeof(int32_t));
        }
        break;
      }

      default:
        DCHECK(false) << "Should have been caught in Prepare()";
    }
  }

 end:
  // The read APIs from stream_ will handle some file corruption issues. For example,
  // if the file is cutoff halfway through a record, the reads will fail and error
  // reported.
  return parse_status_.ok();
}

// The scan node will automatically spin up multiple threads. Each thread uses its own
// Scanner object so ProcessSplit() does not have to think about synchronization.
Status ExampleScanner::ProcessSplit() {
  // Process this entire range.
  while (true) {
    // Impala process tuples in batches (RowBatch). We'll materialize into one row
    // batch at a time.
    MemPool* pool = NULL;
    Tuple* current_tuple = NULL;
    TupleRow* current_row = NULL;

    // This call fills in pool, current_tuple and current_row with where the materialized
    // tuples should go. We can safely write to up to max_tuples before calling this
    // again.
    int max_tuples = GetMemory(&pool, &current_tuple, &current_row);
    DCHECK(pool != NULL);
    DCHECK(current_tuple != NULL);
    DCHECK(current_row != NULL);

    // Keep track of the number of rows that we're adding. This is only updated for
    // tuples that passed the conjuncts.
    int num_to_commit = 0;
    while (num_to_commit < max_tuples) {
      bool success = MaterializeRecord(current_tuple, pool);
      if (!success) break;

      // Put the tuple in the current row and run the conjuncts over it.
      current_row->SetTuple(scan_node_->tuple_idx(), current_tuple);
      if (ExecNode::EvalConjuncts(&(*conjuncts_)[0], num_conjuncts_, current_row)) {
        // The row passed, it needs to be returned. Advance current_tuple and
        // current_row to the next memory location.
        ++num_to_commit;
        current_tuple = next_tuple(current_tuple);
        current_row = next_row(current_row);
      }
    }
    // Update the counter of the number of rows read. This *includes* rows that were
    // filtered out by the conjuncts.
    COUNTER_UPDATE(scan_node_->rows_read_counter(), max_tuples);

    // Commit the rows we've found. This will also check for OOM or cancellation
    // (hence the RETURN_IF_ERROR).
    RETURN_IF_ERROR(CommitRows(num_to_commit));

    // Check exit conditions.
    if (stream_->eosr()) break; // we've finished the entire range.
    if (scan_node_->ReachedLimit()) break; // Reached the limit clause, stop.
    if (!parse_status_.ok()) break; // Ran into an error, time to bail.
  }

  return parse_status_;
}

void ExampleScanner::Close() {
  // AttachPool(my_pool);

  // Add the final row batch to the outgoing queue. If tuples generated by this scanner
  // contain memory that is still owned by this scanner, those resources must be attached
  // before calling AddFinalBatch().
  // This usually happens if the scanner needs to allocate additional storage for string
  // data. If the data buffer from HDFS is not directly consumable (e.g. compressed,
  // custom encoded), the scanner will have to allocated the staging buffers.
  AddFinalRowBatch();

  // Indicate we are done with this range.
  scan_node_->RangeComplete(
      THdfsFileFormat::EXAMPLE_SCANNER_FILE, THdfsCompression::NONE);

  // Close the parent resources.
  HdfsScanner::Close();
}

