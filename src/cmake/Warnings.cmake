# Warnings-as-errors for our own targets only. Anything vendored or fetched is
# pinned upstream code and its diagnostics are not ours to fix.

function(mc_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX
            /w44242  # conversion, possible loss of data
            /w44254
            /w44287 /w44289
            /w44296  # expression is always true/false
            /w44826  # sign-extended conversion
            /w44905 /w44906
            /utf-8
        )
        # Transcriptions narrow deliberately and everywhere: that is what
        # transcribing fixed-width arithmetic means, so the blanket
        # narrowing warnings are noise.
        target_compile_options(${target} PRIVATE /wd4244 /wd4267)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-unused-parameter
            # A partly initialised table or descriptor (the rest zero) is how the
            # machines and chips are declared; MSVC has no such warning either.
            -Wno-missing-field-initializers
        )
    endif()
endfunction()
