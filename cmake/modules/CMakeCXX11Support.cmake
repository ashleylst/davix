include(CheckCXXSourceCompiles REQUIRED)


if(CMAKE_COMPILER_IS_GNUCXX)
   execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
   if (GCC_VERSION VERSION_GREATER 4.7 OR GCC_VERSION VERSION_EQUAL 4.7)
		SET(HAVE_CXX011_FULL_SUPPORT TRUE)
		SET(HAVE_CXX011_PARTIAL_SUPPORT TRUE) 
                SET(CXX11_FLAG_ENABLE "-std=gnu++11")
   elseif(GCC_VERSION VERSION_GREATER 4.3 OR GCC_VERSION VERSION_EQUAL 4.3)
        message(STATUS "C++11 partial support")
		SET(HAVE_CXX011_PARTIAL_SUPPORT TRUE)   
                SET(CXX11_FLAG_ENABLE "-std=gnu++0x")
   else ()
        message(STATUS "C++11 no support ")
        SET(CXX11_FLAG_ENABLE "")
   endif()
else(CMAKE_COMPILER_IS_GNUCXX)
   message(STATUS "C++11 activated full")
   SET(HAVE_CXX011_FULL_SUPPORT TRUE)
   SET(HAVE_CXX011_PARTIAL_SUPPORT TRUE)   
   SET(CXX11_FLAG_ENABLE "-std=c++0x")
endif(CMAKE_COMPILER_IS_GNUCXX)


## Check TR1
CHECK_CXX_SOURCE_COMPILES("
#include <tr1/functional>
int main() { return 0; }"
 HAVE_TR1_SUPPORT)

if(HAVE_TR1_SUPPORT)
message(STATUS "TR1 support detected")
else(HAVE_TR1_SUPPORT)
message(STATUS "no TR1 support")
endif(HAVE_TR1_SUPPORT)
