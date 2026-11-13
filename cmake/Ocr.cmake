# The putgd-ocr text recognition helper.
# Text recognition and QR codes (#1428): Tesseract and ZXing-cpp live in this
# temporary process only; the resident, putgd-ui, putgd-record and the workers
# never link or load them. AUTO builds the helper when both libraries are
# present, as a developer build does for PUTGD_RECORDING; every package build
# sets ON, and a package build without the libraries fails.
set(PUTGD_OCR "AUTO" CACHE STRING "Build the putgd-ocr text recognition helper: AUTO (when Tesseract and ZXing-cpp are present), ON (required) or OFF")
set_property(CACHE PUTGD_OCR PROPERTY STRINGS AUTO ON OFF)
if(NOT PUTGD_OCR MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "PUTGD_OCR must be AUTO, ON or OFF")
endif()
