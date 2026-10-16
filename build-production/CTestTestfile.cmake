# CMake generated Testfile for 
# Source directory: /root/ymconnect/dojo_new
# Build directory: /root/ymconnect/dojo_new/build-production
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(mock_json_dry_run "/root/ymconnect/dojo_new/build-production/rms_collector" "--config" "/root/ymconnect/dojo_new/config/collector.conf.example" "--mock" "--once" "--dry-run")
set_tests_properties(mock_json_dry_run PROPERTIES  _BACKTRACE_TRIPLES "/root/ymconnect/dojo_new/CMakeLists.txt;39;add_test;/root/ymconnect/dojo_new/CMakeLists.txt;0;")
