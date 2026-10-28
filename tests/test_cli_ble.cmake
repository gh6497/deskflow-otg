# Parser rejection only. No valid service, adapter or HID request is started.
function(reject)
  execute_process(COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
  if(NOT code STREQUAL "2")
    message(FATAL_ERROR "Expected argument error for ${ARGN}: ${code} ${out} ${err}")
  endif()
endfunction()
reject(--transport ble --ble-session)
reject(--transport ble --ble-peer invalid)
reject(--ble-status --adapter hci0/../../x)
reject(--ble-status --adapter hci65535)
reject(--ble-status --ble-stop-service)
reject(--ble-service --gui)
reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --ble-session --mouse-mode relative)
if(WINDOWS_BLE)
  reject(--gui-ble-control)
  reject(--gui --gui-ble-control --serial test)
  reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --mouse-mode relative)
  reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --adapter hci0)
  reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --serial test)
  reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --gui --background)
  reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --ble-isolate)
  reject(--transport ble --ble-peer AA:BB:CC:DD:EE:FF --ble-session)
  # EOF cancels GUI startup before opening Bluetooth. Exercise the real parser
  # and stdin stop path without accessing any adapter or modifying pairings.
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ble-gui-eof.txt" "")
  execute_process(COMMAND "${CLI}" --transport ble --ble-peer AA:BB:CC:DD:EE:FF --gui --gui-ble-control
    INPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/ble-gui-eof.txt"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
  if(NOT code STREQUAL "0")
    message(FATAL_ERROR "GUI BLE EOF cancellation failed: ${code} ${out} ${err}")
  endif()
endif()
