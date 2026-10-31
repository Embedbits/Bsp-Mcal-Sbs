#======================== Unit tests configuration ============================#
# Unit tests of Sbs module. Registered in module CMakeLists.txt by
# UnitTesting_AddPath() and created by UnitTesting_Generate() in "UnitTest"
# build (see EmBi_Platform/CMake/UnitTesting/README.md).

get_filename_component(SBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(MCAL_DIR "${SBS_DIR}/.." ABSOLUTE)

# The RCC module (clock of the SBS, CSI oscillator, clock of the Ethernet MAC) is mocked. Tests of the
# read-back verification and of the compensation cell sequence use the HW model of RegMem (register value
# which does not stay as written, ready flag of the cell) - executed serially where the model runs in a thread.
# SBS (0x44000400, secure alias 0x54000400) is inside of the peripheral region of RegMem.
UnitTesting_Add_Test(
    NAME            Sbs
    TEST_SOURCE     Test_Sbs.c
    UUT_SOURCES     ${SBS_DIR}/Sbs.c
                    ${SBS_DIR}/Sbs_Reg.c
    MOCK_HEADERS    ${MCAL_DIR}/Rcc/Rcc_Port.h
    INCLUDE_DIRS    ${SBS_DIR}
                    ${MCAL_DIR}/Rcc
    LINK_LIBS       ut_RegMem                           # Registers in host memory
                    ut_CmsisHost                        # CMSIS core intrinsics on host
    FORCE_INCLUDES  ${UNIT_TESTING_CMSIS_HOST_HEADER}   # Replaces cmsis_gcc.h
    HOST_LIBS       Ral_Lib                             # ST LL compiled for host
    COVERAGE_EXCLUDE ".*/Ral/.*"
    RUN_SERIAL
)
