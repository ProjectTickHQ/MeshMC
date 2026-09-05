if(__QMAKEQUERY_CMAKE__)
    return()
endif()
set(__QMAKEQUERY_CMAKE__ TRUE)

get_target_property(QMAKE_EXECUTABLE Qt${QT_VERSION_MAJOR}::qmake LOCATION)

function(QUERY_QMAKE VAR RESULT)
    execute_process(COMMAND ${QMAKE_EXECUTABLE} -query ${VAR}
        RESULT_VARIABLE return_code
        OUTPUT_VARIABLE output
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(return_code EQUAL 0)
        file(TO_CMAKE_PATH "${output}" output)
        set(${RESULT} ${output} PARENT_SCOPE)
    endif()
endfunction(QUERY_QMAKE)
