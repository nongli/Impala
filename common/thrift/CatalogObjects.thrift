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

namespace cpp impala
namespace java com.cloudera.impala.thrift

include "Exprs.thrift"
include "Status.thrift"
include "Types.thrift"
include "hive_metastore.thrift"

// Types used to represent catalog objects.

// Type of Catalog object.
enum TCatalogObjectType {
  // UNKNOWN is used to indicate an error condition when converting
  // strings to their matching TCatalogObjectType.
  UNKNOWN,
  CATALOG,
  DATABASE,
  TABLE,
  VIEW,
  FUNCTION,
}

enum TTableType {
  HDFS_TABLE,
  HBASE_TABLE,
  VIEW,
  // A table that does not contain all needed metadata. This can be either because
  // of an error loading the metadata or because the table metadata has not yet
  // been loaded.
  INCOMPLETE_TABLE
}

enum THdfsFileFormat {
  TEXT,
  LZO_TEXT,
  RC_FILE,
  SEQUENCE_FILE,
  AVRO,
  PARQUET,
  EXAMPLE_SCANNER_FILE,
}

enum THdfsCompression {
  NONE,
  DEFAULT,
  GZIP,
  DEFLATE,
  BZIP2,
  SNAPPY,
  SNAPPY_BLOCKED, // Used by sequence and rc files but not stored in the metadata.
  LZO
}

// The table property type.
enum TTablePropertyType {
  TBL_PROPERTY,
  SERDE_PROPERTY
}

// The access level that is available to Impala on the Catalog object.
enum TAccessLevel {
  NONE,
  READ_WRITE,
  READ_ONLY,
  WRITE_ONLY,
}

// Mapping from names defined by Avro to values in the THdfsCompression enum.
const map<string, THdfsCompression> COMPRESSION_MAP = {
  "": THdfsCompression.NONE,
  "none": THdfsCompression.NONE,
  "deflate": THdfsCompression.DEFAULT,
  "gzip": THdfsCompression.GZIP,
  "bzip2": THdfsCompression.BZIP2,
  "snappy": THdfsCompression.SNAPPY
}

// Represents a single item in a partition spec (column name + value)
struct TPartitionKeyValue {
  // Partition column name
  1: required string name,

  // Partition value
  2: required string value
}

// Represents a fully qualified table name.
struct TTableName {
  // Name of the table's parent database.
  1: required string db_name

  // Name of the table
  2: required string table_name
}

struct TTableStats {
  // Estimated number of rows in the table or -1 if unknown
  1: required i64 num_rows;
}

// Column stats data that Impala uses.
struct TColumnStats {
  // Average size and max size, in bytes. Excludes serialization overhead.
  // For fixed-length types (those which don't need additional storage besides the slot
  // they occupy), sets avg_size and max_size to their slot size.
  1: required double avg_size
  2: required i64 max_size

  // Estimated number of distinct values.
  3: required i64 num_distinct_values

  // Estimated number of null values.
  4: required i64 num_nulls
}

struct TColumn {
  1: required string columnName
  2: required Types.TColumnType columnType
  3: optional string comment
  // Stats for this table, if any are available.
  4: optional TColumnStats col_stats
  // Ordinal position in the source table
  5: optional i32 position

  // Indicates whether this is an HBase column. If true, implies
  // all following HBase-specific fields are set.
  6: optional bool is_hbase_column
  7: optional string column_family
  8: optional string column_qualifier
  9: optional bool is_binary
}

// Represents a block in an HDFS file
struct THdfsFileBlock {
  // Name of the file
  1: required string file_name

  // Size of the file
  2: required i64 file_size

  // Offset of this block within the file
  3: required i64 offset

  // Total length of the block
  4: required i64 length

  // List of datanodes network addresses (IP address and port) that contain this block
  5: required list<Types.TNetworkAddress> network_addresses

  // The list of disk ids for the file block. May not be set if disk ids are not supported
  6: optional list<i32> disk_ids
}

// Represents an HDFS file
struct THdfsFileDesc {
  1: required string path
  2: required i64 length
  3: required THdfsCompression compression
  4: required i64 last_modification_time
  5: required list<THdfsFileBlock> file_blocks
}

// Represents an HDFS partition
struct THdfsPartition {
  1: required byte lineDelim
  2: required byte fieldDelim
  3: required byte collectionDelim
  4: required byte mapKeyDelim
  5: required byte escapeChar
  6: required THdfsFileFormat fileFormat
  7: list<Exprs.TExpr> partitionKeyExprs
  8: required i32 blockSize
  9: required THdfsCompression compression
  10: optional list<THdfsFileDesc> file_desc
  11: optional string location

  // The access level Impala has on this partition (READ_WRITE, READ_ONLY, etc).
  12: optional TAccessLevel access_level

  // Statistics on this partition, e.g., number of rows in this partition.
  13: optional TTableStats stats
}

struct THdfsTable {
  1: required string hdfsBaseDir

  // Names of the columns, including clustering columns.  As in other
  // places, the clustering columns come before the non-clustering
  // columns.  This includes non-materialized columns.
  2: required list<string> colNames;

  // The string used to represent NULL partition keys.
  3: required string nullPartitionKeyValue

  // String to indicate a NULL column value in text files
  5: required string nullColumnValue

  // Set to the table's Avro schema if this is an Avro table
  6: optional string avroSchema

  // map from partition id to partition metadata
  4: required map<i64, THdfsPartition> partitions
}

struct THBaseTable {
  1: required string tableName
  2: required list<string> families
  3: required list<string> qualifiers

  // Column i is binary encoded if binary_encoded[i] is true. Otherwise, column i is
  // text encoded.
  4: optional list<bool> binary_encoded
}

// Represents a table or view.
struct TTable {
  // Name of the parent database
  1: required string db_name

  // Unqualified table name
  2: required string tbl_name

  // Set if there were any errors loading the Table metadata. The remaining fields in
  // the struct may not be set if there were problems loading the table metadata.
  // By convention, the final error message in the Status should contain the call stack
  // string pointing to where the metadata loading error occurred.
  3: optional Status.TStatus load_status

  // Table identifier.
  4: optional Types.TTableId id

  // The access level Impala has on this table (READ_WRITE, READ_ONLY, etc).
  5: optional TAccessLevel access_level

  // List of columns (excludes clustering columns)
  6: optional list<TColumn> columns

  // List of clustering columns (empty list if table has no clustering columns)
  7: optional list<TColumn> clustering_columns

  // Table stats data for the table.
  8: optional TTableStats table_stats

  // Determines the table type - either HDFS, HBASE, or VIEW.
  9: optional TTableType table_type

  // Set iff this is an HDFS table
  10: optional THdfsTable hdfs_table

  // Set iff this is an Hbase table
  11: optional THBaseTable hbase_table

  // The Hive Metastore representation of this table. May not be set if there were
  // errors loading the table metadata
  12: optional hive_metastore.Table metastore_table
}

// Represents a database.
struct TDatabase {
  // Name of the database
  1: required string db_name

  // The HDFS location new tables will default their base directory to
  2: optional string location
}

// Represents state associated with the overall catalog.
struct TCatalog {
  // The CatalogService service ID.
  1: required Types.TUniqueId catalog_service_id
}

// Union of all Thrift Catalog objects
struct TCatalogObject {
  // The object type (Database, Table, View, or Function)
  1: required TCatalogObjectType type

  // The Catalog version this object is from
  2: required i64 catalog_version

  // Set iff object type is CATALOG
  3: optional TCatalog catalog

  // Set iff object type is DATABASE
  4: optional TDatabase db

  // Set iff object type is TABLE or VIEW
  5: optional TTable table

  // Set iff object type is FUNCTION
  6: optional Types.TFunction fn
}
