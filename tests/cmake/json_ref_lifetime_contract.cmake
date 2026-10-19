cmake_minimum_required(VERSION 3.24)
# QJsonValue::toObject()/toArray() and QJsonDocument::object()/array() return a
# temporary container. Its non-const operator[] returns a QJsonValueRef into
# that temporary, so an auto variable initialized from it outlives what it
# refers to. Settings::validate crashed the Debian 13 resident this way. GCC's
# AddressSanitizer does not poison full-expression temporaries and reported
# nothing, so this contract rejects the pattern in source instead. Use
# .value(key) / .at(index), or convert in the same expression.
if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT is required")
endif()
set(pattern [==[(^|[^A-Za-z0-9_])auto[ &]+[A-Za-z_][A-Za-z0-9_]*[ ]*=[^;{}]*\.(toObject|toArray|object|array)\(\)[ ]*\[[^;{}]*\][ ]*[;:]]==])
function(matches text output)
  string(REGEX MATCH "${pattern}" found "${text}")
  set(${output} "${found}" PARENT_SCOPE)
endfunction()
# The rule must still recognize the original defects and accept their fixes.
foreach(bad
    [==[const auto value = doc["actions"].toObject()[name];]==]
    [==[auto value=draft_["limits"].toObject().contains(k)?draft_["limits"].toObject()[k]:draft_["retention"].toObject()[k];]==]
    [==[const auto& first = QJsonDocument::fromJson(bytes).array()[0];]==])
  matches("${bad}" found)
  if(found STREQUAL "")
    message(FATAL_ERROR "The lifetime rule no longer matches: ${bad}")
  endif()
endforeach()
foreach(good
    [==[const QJsonValue value = doc["actions"].toObject().value(name);]==]
    [==[auto action=doc["actions"].toObject()[name].toObject();]==]
    [==[const auto id=profiles[index].toObject()["id"].toString();]==]
    [==[const QJsonValue value=retention?defaults["retention"].toObject()[key]:defaults["limits"].toObject()[key];]==]
    [==[if(!folder(archive["folders"].toObject()[category], true)) return;]==])
  matches("${good}" found)
  if(NOT found STREQUAL "")
    message(FATAL_ERROR "The lifetime rule rejects a safe expression: ${good}")
  endif()
endforeach()
file(GLOB_RECURSE sources LIST_DIRECTORIES false RELATIVE "${REPO_ROOT}"
  "${REPO_ROOT}/src/*.cpp" "${REPO_ROOT}/src/*.h" "${REPO_ROOT}/tests/*.cpp" "${REPO_ROOT}/tests/*.h")
set(failures "")
foreach(source IN LISTS sources)
  file(READ "${REPO_ROOT}/${source}" content)
  # Statements may span lines; a space keeps every offset for line numbers.
  string(REPLACE "\n" " " flat "${content}")
  string(REGEX MATCHALL "${pattern}" found "${flat}")
  # A match's closing ";" splits it into a list element and an empty one.
  foreach(match IN LISTS found)
    string(REGEX REPLACE "^[^A-Za-z0-9_]" "" match "${match}")
    if(match STREQUAL "")
      continue()
    endif()
    string(FIND "${flat}" "${match}" offset)
    string(SUBSTRING "${content}" 0 ${offset} before)
    string(REGEX MATCHALL "\n" breaks "${before}")
    list(LENGTH breaks line)
    math(EXPR line "${line} + 1")
    string(APPEND failures "\n  ${source}:${line}: ${match}")
  endforeach()
endforeach()
if(NOT failures STREQUAL "")
  message(FATAL_ERROR "auto variables keep a QJsonValueRef into a destroyed temporary:${failures}")
endif()
list(LENGTH sources count)
message(STATUS "No auto variable keeps a reference into a temporary JSON container (${count} files).")
