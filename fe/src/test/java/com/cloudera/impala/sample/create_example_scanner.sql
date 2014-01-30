create table example_scanner_tbl(
  col1 int
)
ROW FORMAT DELIMITED FIELDS TERMINATED BY '\t'
STORED AS INPUTFORMAT 'com.cloudera.impala.sample.ExampleScannerInputFormat'
OUTPUTFORMAT 'com.cloudera.impala.sample.ExampleScannerOutputFormat';
