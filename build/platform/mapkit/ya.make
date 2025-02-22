RESOURCES_LIBRARY()



NO_PLATFORM()

IF (OS_ANDROID)
    DECLARE_EXTERNAL_RESOURCE(MAPKIT_ANDROID_LIBCXX_HEADERS sbr:1114287237)

    CFLAGS(GLOBAL -nostdinc++ GLOBAL -cxx-isystem GLOBAL $MAPKIT_ANDROID_LIBCXX_HEADERS_RESOURCE_GLOBAL)

    DECLARE_EXTERNAL_RESOURCE(MAPKIT_ANDROID_LIBCXX_LIBRARIES sbr:1116167275)

    IF (ARCH_ARM7)
        SET(ARCH_NAME arm)
    ELSEIF (ARCH_ARM64)
        SET(ARCH_NAME arm64)
    ELSEIF (ARCH_I686)
        SET(ARCH_NAME x86)
    ELSEIF (ARCH_X86_64)
        SET(ARCH_NAME x86-64)
    ENDIF()

    SET(LIBS $MAPKIT_ANDROID_LIBCXX_LIBRARIES_RESOURCE_GLOBAL/$ARCH_NAME)

    LDFLAGS(-L$LIBS)

    IF (ARCH_ARM64)
        LDFLAGS(-lunwind)
    ENDIF()

    LDFLAGS($LIBS/libc++_shared.so)
ENDIF()

END()
