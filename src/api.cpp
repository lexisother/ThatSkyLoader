#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <libmem.h>
#include "include/api.h"

ModApi* ModApi::instance = NULL;

ModApi& ModApi::Instance() {
    if(instance == NULL) instance = new ModApi;
    return *instance;
}

ModApi::ModApi() {
    skyBase = 0;
}

lm_module_t mod;
void ModApi::InitSkyBase() {
    while(LM_LoadModule("Sky.exe", &mod) == 0) Sleep(100);
    skyBase = mod.base;
    skySize = mod.size;
}

uintptr_t ModApi::GetSkyBase() {
	return skyBase;
}

uintptr_t ModApi::GetSkySize() {
    return skySize;
}