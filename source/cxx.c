/**
 * @file cxx.c
 * @brief Guest libstdc++ / libsupc++ imports bound to the VitaSDK libstdc++ (old COW std::string ABI).
 * @details iOS GCC 4.2 and VitaSDK libstdc++ share the Itanium C++ ABI and 32-bit layouts; only the
 *          mangling of size_t differs (iOS: unsigned long 'm', Vita: unsigned int 'j').
 *          Symbols are referenced through asm labels so the C compiler never needs their C++ types.
 *          Generated from the guest import list checked against `nm libstdc++.a`.
 */
#include "imports.h"

extern char cxx_f0[] __asm__("_ZN9__gnu_cxx18__exchange_and_addEPVii");
extern char cxx_f1[] __asm__("_ZNKSs4findEPKcj");
extern char cxx_f2[] __asm__("_ZNKSs6substrEjj");
extern char cxx_f3[] __asm__("_ZNKSs7compareEPKc");
extern char cxx_f4[] __asm__("_ZNKSs7compareERKSs");
extern char cxx_f5[] __asm__("_ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv");
extern char cxx_f6[] __asm__("_ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc");
extern char cxx_f7[] __asm__("_ZNSo9_M_insertIlEERSoT_");
extern char cxx_f8[] __asm__("_ZNSolsEi");
extern char cxx_f9[] __asm__("_ZNSs2atEj");
extern char cxx_f10[] __asm__("_ZNSs4_Rep10_M_destroyERKSaIcE");
extern char cxx_f11[] __asm__("_ZNSs6appendEPKcj");
extern char cxx_f12[] __asm__("_ZNSs6appendERKSs");
extern char cxx_f13[] __asm__("_ZNSs6appendEjc");
extern char cxx_f14[] __asm__("_ZNSs6assignEPKcj");
extern char cxx_f15[] __asm__("_ZNSs6assignERKSs");
extern char cxx_f16[] __asm__("_ZNSs6insertEjRKSsjj");
extern char cxx_f17[] __asm__("_ZNSs6resizeEjc");
extern char cxx_f18[] __asm__("_ZNSs7reserveEj");
extern char cxx_f19[] __asm__("_ZNSs9_M_mutateEjjj");
extern char cxx_f20[] __asm__("_ZNSs9push_backEc");
extern char cxx_f21[] __asm__("_ZNSsC1EPKcRKSaIcE");
extern char cxx_f22[] __asm__("_ZNSsC1EPKcjRKSaIcE");
extern char cxx_f23[] __asm__("_ZNSsC1ERKSs");
extern char cxx_f24[] __asm__("_ZNSsD2Ev");
extern char cxx_f25[] __asm__("_ZNSsixEj");
extern char cxx_f26[] __asm__("_ZNSt15_List_node_base4hookEPS_");
extern char cxx_f27[] __asm__("_ZNSt15_List_node_base4swapERS_S0_");
extern char cxx_f28[] __asm__("_ZNSt15_List_node_base6unhookEv");
extern char cxx_f29[] __asm__("_ZNSt15_List_node_base8transferEPS_S0_");
extern char cxx_f30[] __asm__("_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode");
extern char cxx_f31[] __asm__("_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev");
extern char cxx_f32[] __asm__("_ZNSt8ios_base4InitC1Ev");
extern char cxx_f33[] __asm__("_ZNSt8ios_base4InitD1Ev");
extern char cxx_f34[] __asm__("_ZSt17__throw_bad_allocv");
extern char cxx_f35[] __asm__("_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base");
extern char cxx_f36[] __asm__("_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base");
extern char cxx_f37[] __asm__("_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base");
extern char cxx_f38[] __asm__("_ZSt20__throw_length_errorPKc");
extern char cxx_f39[] __asm__("_ZSt20__throw_out_of_rangePKc");
extern char cxx_f40[] __asm__("_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_");
extern char cxx_f41[] __asm__("_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_");
extern char cxx_f42[] __asm__("_ZdaPv");
extern char cxx_f43[] __asm__("_ZdlPvRKSt9nothrow_t");
extern char cxx_f44[] __asm__("_Znaj");
extern char cxx_f45[] __asm__("_Znwj");
extern char cxx_f46[] __asm__("_ZnwjRKSt9nothrow_t");
extern char cxx_f47[] __asm__("__cxa_begin_catch");
extern char cxx_f48[] __asm__("__cxa_end_catch");
extern char cxx_f49[] __asm__("__cxa_rethrow");
extern char cxx_f50[] __asm__("__dynamic_cast");
extern char cxx_d0[] __asm__("_ZNSs4_Rep20_S_empty_rep_storageE");
extern char cxx_d1[] __asm__("_ZSt7nothrow");
extern char cxx_d2[] __asm__("_ZTVN10__cxxabiv117__class_type_infoE");
extern char cxx_d3[] __asm__("_ZTVN10__cxxabiv120__si_class_type_infoE");
extern char cxx_d4[] __asm__("_ZTVN10__cxxabiv121__vmi_class_type_infoE");

const import_entry cxx_imports[] = {
    F_RAW("__ZN9__gnu_cxx18__exchange_and_addEPVii", cxx_f0),
    F_RAW("__ZNKSs4findEPKcm", cxx_f1),
    F_RAW("__ZNKSs6substrEmm", cxx_f2),
    F_RAW("__ZNKSs7compareEPKc", cxx_f3),
    F_RAW("__ZNKSs7compareERKSs", cxx_f4),
    F_RAW("__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", cxx_f5),
    F_RAW("__ZNKSt9basic_iosIcSt11char_traitsIcEE5widenEc", cxx_f6),
    F_RAW("__ZNSo9_M_insertIlEERSoT_", cxx_f7),
    F_RAW("__ZNSolsEi", cxx_f8),
    F_RAW("__ZNSs2atEm", cxx_f9),
    F_RAW("__ZNSs4_Rep10_M_destroyERKSaIcE", cxx_f10),
    F_RAW("__ZNSs6appendEPKcm", cxx_f11),
    F_RAW("__ZNSs6appendERKSs", cxx_f12),
    F_RAW("__ZNSs6appendEmc", cxx_f13),
    F_RAW("__ZNSs6assignEPKcm", cxx_f14),
    F_RAW("__ZNSs6assignERKSs", cxx_f15),
    F_RAW("__ZNSs6insertEmRKSsmm", cxx_f16),
    F_RAW("__ZNSs6resizeEmc", cxx_f17),
    F_RAW("__ZNSs7reserveEm", cxx_f18),
    F_RAW("__ZNSs9_M_mutateEmmm", cxx_f19),
    F_RAW("__ZNSs9push_backEc", cxx_f20),
    F_RAW("__ZNSsC1EPKcRKSaIcE", cxx_f21),
    F_RAW("__ZNSsC1EPKcmRKSaIcE", cxx_f22),
    F_RAW("__ZNSsC1ERKSs", cxx_f23),
    F_RAW("__ZNSsD2Ev", cxx_f24),
    F_RAW("__ZNSsixEm", cxx_f25),
    F_RAW("__ZNSt15_List_node_base4hookEPS_", cxx_f26),
    F_RAW("__ZNSt15_List_node_base4swapERS_S0_", cxx_f27),
    F_RAW("__ZNSt15_List_node_base6unhookEv", cxx_f28),
    F_RAW("__ZNSt15_List_node_base8transferEPS_S0_", cxx_f29),
    F_RAW("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", cxx_f30),
    F_RAW("__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", cxx_f31),
    F_RAW("__ZNSt8ios_base4InitC1Ev", cxx_f32),
    F_RAW("__ZNSt8ios_base4InitD1Ev", cxx_f33),
    F_RAW("__ZSt17__throw_bad_allocv", cxx_f34),
    F_RAW("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", cxx_f35),
    F_RAW("__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", cxx_f36),
    F_RAW("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", cxx_f37),
    F_RAW("__ZSt20__throw_length_errorPKc", cxx_f38),
    F_RAW("__ZSt20__throw_out_of_rangePKc", cxx_f39),
    F_RAW("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", cxx_f40),
    F_RAW("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", cxx_f41),
    F_RAW("__ZdaPv", cxx_f42),
    F_RAW("__ZdlPvRKSt9nothrow_t", cxx_f43),
    F_RAW("__Znam", cxx_f44),
    F_RAW("__Znwm", cxx_f45),
    F_RAW("__ZnwmRKSt9nothrow_t", cxx_f46),
    F_RAW("___cxa_begin_catch", cxx_f47),
    F_RAW("___cxa_end_catch", cxx_f48),
    F_RAW("___cxa_rethrow", cxx_f49),
    F_RAW("___dynamic_cast", cxx_f50),
    {"__ZNSs4_Rep20_S_empty_rep_storageE", (uintptr_t)cxx_d0, 1},
    {"__ZSt7nothrow", (uintptr_t)cxx_d1, 1},
    {"__ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)cxx_d2, 1},
    {"__ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)cxx_d3, 1},
    {"__ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)cxx_d4, 1},
};
const int cxx_imports_count = sizeof(cxx_imports) / sizeof(cxx_imports[0]);
