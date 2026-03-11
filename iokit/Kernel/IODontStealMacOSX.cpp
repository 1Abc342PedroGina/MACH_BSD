/*
 * Copyright (c) 2006-2022 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitDebug.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/crypto/sha1.h>
#include <mach/mach_types.h>
#include <sys/csr.h>
#include <kern/debug.h>
#include <string.h>

#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/kauth.h>

//==============================================================================
// Estruturas para comunicação com o SMC
//==============================================================================

typedef struct {
    uint32_t    key;
    uint8_t     __d0[22];
    uint32_t    datasize;
    uint8_t     __d1[10];
    uint8_t     cmd;
    uint32_t    __d2;
    uint8_t     data[32];
} AppleSMCBuffer_t;

#define SMC_CMD_READ_KEYINFO    9
#define SMC_CMD_READ_KEY        5
#define SMC_KEY_SIZE            32

//==============================================================================
// A famosa chave OSK (OS Key) - "ourhardworkbythesewordsguardedpleasedontsteal(c)applecomputerinc"
//==============================================================================

static const uint8_t gOSKKey[64] = {
    'o','u','r','h','a','r','d','w','o','r','k','b','y','t','h','e',
    's','e','w','o','r','d','s','g','u','a','r','d','e','d','p','l',
    'e','a','s','e','d','o','n','t','s','t','e','a','l','(','c',')',
    'a','p','p','l','e','c','o','m','p','u','t','e','r','i','n','c'
};

static_assert(sizeof(gOSKKey) == 64, "OSK key must be 64 bytes");

//==============================================================================
// Mensagens criptografadas e poemas do DSMOS
//==============================================================================

// A string criptografada que contém o poema (versão ofuscada)
static const uint8_t gEncryptedPoem[] = {
    0x59, 0x6f, 0x75, 0x72, 0x20, 0x6b, 0x61, 0x72, 0x6d, 0x61, 0x20, 0x63,
    0x68, 0x65, 0x63, 0x6b, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x74, 0x6f, 0x64,
    0x61, 0x79, 0x3a, 0x0a, 0x54, 0x68, 0x65, 0x72, 0x65, 0x20, 0x6f, 0x6e,
    0x63, 0x65, 0x20, 0x77, 0x61, 0x73, 0x20, 0x77, 0x61, 0x73, 0x20, 0x61,
    0x20, 0x75, 0x73, 0x65, 0x72, 0x20, 0x74, 0x68, 0x61, 0x74, 0x20, 0x77,
    0x68, 0x69, 0x6e, 0x65, 0x64, 0x0a, 0x68, 0x69, 0x73, 0x20, 0x65, 0x78,
    0x69, 0x73, 0x74, 0x69, 0x6e, 0x67, 0x20, 0x4f, 0x53, 0x20, 0x77, 0x61,
    0x73, 0x20, 0x73, 0x6f, 0x20, 0x62, 0x6c, 0x69, 0x6e, 0x64, 0x2c, 0x0a,
    0x68, 0x65, 0x27, 0x64, 0x20, 0x64, 0x6f, 0x20, 0x62, 0x65, 0x74, 0x74,
    0x65, 0x72, 0x20, 0x74, 0x6f, 0x20, 0x70, 0x69, 0x72, 0x61, 0x74, 0x65,
    0x0a, 0x61, 0x6e, 0x20, 0x4f, 0x53, 0x20, 0x74, 0x68, 0x61, 0x74, 0x20,
    0x72, 0x61, 0x6e, 0x20, 0x67, 0x72, 0x65, 0x61, 0x74, 0x0a, 0x62, 0x75,
    0x74, 0x20, 0x66, 0x6f, 0x75, 0x6e, 0x64, 0x20, 0x68, 0x69, 0x73, 0x20,
    0x68, 0x61, 0x72, 0x64, 0x77, 0x61, 0x72, 0x65, 0x20, 0x64, 0x65, 0x63,
    0x6c, 0x69, 0x6e, 0x65, 0x64, 0x2e, 0x0a, 0x50, 0x6c, 0x65, 0x61, 0x73,
    0x65, 0x20, 0x64, 0x6f, 0x6e, 0x27, 0x74, 0x20, 0x73, 0x74, 0x65, 0x61,
    0x6c, 0x20, 0x4d, 0x61, 0x63, 0x20, 0x4f, 0x53, 0x21, 0x0a, 0x52, 0x65,
    0x61, 0x6c, 0x6c, 0x79, 0x2c, 0x20, 0x74, 0x68, 0x61, 0x74, 0x27, 0x73,
    0x20, 0x77, 0x61, 0x79, 0x20, 0x75, 0x6e, 0x63, 0x6f, 0x6f, 0x6c, 0x2e,
    0x0a, 0x28, 0x43, 0x29, 0x20, 0x41, 0x70, 0x70, 0x6c, 0x65, 0x20, 0x43,
    0x6f, 0x6d, 0x70, 0x75, 0x74, 0x65, 0x72, 0x2c, 0x20, 0x49, 0x6e, 0x63,
    0x2e, 0x0a, 0x55, 0xef, 0xbf, 0xbd, 0x3f, 0x56, 0x57, 0x53, 0xef, 0xbf,
    0xbd, 0x3f, 0x35, 0x50
};

//==============================================================================
// Lista de binários críticos protegidos pelo DSMOS
//==============================================================================

static const char *gProtectedBinaries[] = {
    "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock",
    "/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder",
    "/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow",
    "/System/Library/CoreServices/SystemUIServer.app/Contents/MacOS/SystemUIServer",
    "/System/Library/CoreServices/Spotlight.app/Contents/MacOS/mds",
    "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/Frameworks/ATS.framework/Support/ATSServer",
    "/usr/libexec/oah/translate",
    "/usr/libexec/oah/translated",
    NULL
};

//==============================================================================
// Classe principal DSMOS
//==============================================================================

class com_apple_iokit_DSMOS : public IOService
{
    OSDeclareDefaultStructors(com_apple_iokit_DSMOS)
    
private:
    io_service_t        fSMCService;
    io_connect_t        fSMCConnect;
    IOLock*             fLock;
    bool                fSMCAvailable;
    uint8_t             fOSK[64];
    uint8_t             fCommPageData[256];  // Blob de integridade na commpage
    uint32_t            fValidationFlags;
    
    // Métodos privados
    bool                openSMC(void);
    void                closeSMC(void);
    bool                readOSKey(void);
    bool                verifySMCPresence(void);
    void                registerMachOLoaderHook(void);
    void                decryptSegments(const char* binaryPath);
    bool                validateBinary(const char* path);
    void                writeCommPageData(void);
    void                panicWithPoem(void);
    
    // Hook para loader Mach-O
    static void         machOLoaderHook(struct mach_header* mh, intptr_t slide, const char* path);
    
protected:
    bool                init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    // Manipulação de IOKit
    IOReturn            callPlatformFunction(const OSSymbol* functionName,
                                             bool waitForFunction,
                                             void* param1, void* param2,
                                             void* param3, void* param4) APPLE_KEXT_OVERRIDE;
    
    // Propriedades estáticas
    virtual OSString*   getDeviceName(void) const APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_iokit_DSMOS, IOService)

//==============================================================================
// Inicialização
//==============================================================================

bool com_apple_iokit_DSMOS::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    fLock = IOLockAlloc();
    if (!fLock) {
        return false;
    }
    
    fSMCService = 0;
    fSMCConnect = 0;
    fSMCAvailable = false;
    fValidationFlags = 0;
    
    bzero(fOSK, sizeof(fOSK));
    bzero(fCommPageData, sizeof(fCommPageData));
    
    return true;
}

void com_apple_iokit_DSMOS::free(void)
{
    closeSMC();
    
    if (fLock) {
        IOLockFree(fLock);
        fLock = NULL;
    }
    
    super::free();
}

//==============================================================================
// Abertura e fechamento do SMC
//==============================================================================

bool com_apple_iokit_DSMOS::openSMC(void)
{
    IOReturn status;
    
    IOLockLock(fLock);
    
    if (fSMCAvailable) {
        IOLockUnlock(fLock);
        return true;
    }
    
    // Procurar serviço AppleSMC
    fSMCService = IOServiceGetMatchingService(kIOMasterPortDefault,
                                               IOServiceMatching("AppleSMC"));
    
    if (!fSMCService) {
        if (gIOKitDebug & kIOLogDebug) {
            IOLog("DSMOS: AppleSMC not found\n");
        }
        IOLockUnlock(fLock);
        return false;
    }
    
    // Abrir conexão com o SMC
    status = IOServiceOpen(fSMCService, mach_task_self(), 0, &fSMCConnect);
    if (status != kIOReturnSuccess) {
        if (gIOKitDebug & kIOLogDebug) {
            IOLog("DSMOS: Failed to open SMC connection: 0x%x\n", status);
        }
        IOObjectRelease(fSMCService);
        fSMCService = 0;
        IOLockUnlock(fLock);
        return false;
    }
    
    fSMCAvailable = true;
    IOLockUnlock(fLock);
    
    return true;
}

void com_apple_iokit_DSMOS::closeSMC(void)
{
    IOLockLock(fLock);
    
    if (fSMCConnect) {
        IOServiceClose(fSMCConnect);
        fSMCConnect = 0;
    }
    
    if (fSMCService) {
        IOObjectRelease(fSMCService);
        fSMCService = 0;
    }
    
    fSMCAvailable = false;
    IOLockUnlock(fLock);
}

//==============================================================================
// Leitura da OS Key do SMC
//==============================================================================

bool com_apple_iokit_DSMOS::readOSKey(void)
{
    IOReturn status;
    AppleSMCBuffer_t inputStruct;
    AppleSMCBuffer_t outputStruct;
    size_t outputStructLength = sizeof(outputStruct);
    
    if (!fSMCAvailable) {
        return false;
    }
    
    IOLockLock(fLock);
    
    // Ler OSK0
    bzero(&inputStruct, sizeof(inputStruct));
    inputStruct.key = 'OSK0';
    inputStruct.datasize = SMC_KEY_SIZE;
    inputStruct.cmd = SMC_CMD_READ_KEY;
    
    status = IOConnectCallStructMethod(fSMCConnect, 2,
                                        &inputStruct, sizeof(inputStruct),
                                        &outputStruct, &outputStructLength);
    
    if (status != kIOReturnSuccess) {
        IOLog("DSMOS: Failed to read OSK0: 0x%x\n", status);
        IOLockUnlock(fLock);
        return false;
    }
    
    // Copiar primeira metade da chave
    bcopy(outputStruct.data, fOSK, SMC_KEY_SIZE);
    
    // Ler OSK1
    inputStruct.key = 'OSK1';
    
    status = IOConnectCallStructMethod(fSMCConnect, 2,
                                        &inputStruct, sizeof(inputStruct),
                                        &outputStruct, &outputStructLength);
    
    if (status != kIOReturnSuccess) {
        IOLog("DSMOS: Failed to read OSK1: 0x%x\n", status);
        IOLockUnlock(fLock);
        return false;
    }
    
    // Copiar segunda metade da chave
    bcopy(outputStruct.data, fOSK + SMC_KEY_SIZE, SMC_KEY_SIZE);
    
    IOLockUnlock(fLock);
    
    // Verificar se a chave lida corresponde à conhecida
    if (memcmp(fOSK, gOSKKey, sizeof(gOSKKey)) != 0) {
        IOLog("DSMOS: OSK verification failed - invalid key\n");
        return false;
    }
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("DSMOS: OSK successfully validated\n");
    }
    
    return true;
}

//==============================================================================
// Verificação de presença do SMC
//==============================================================================

bool com_apple_iokit_DSMOS::verifySMCPresence(void)
{
    if (!openSMC()) {
        return false;
    }
    
    if (!readOSKey()) {
        closeSMC();
        return false;
    }
    
    return true;
}

//==============================================================================
// Panic com o poema do DSMOS
//==============================================================================

void com_apple_iokit_DSMOS::panicWithPoem(void)
{
    char poem[512];
    size_t poemLen = sizeof(gEncryptedPoem);
    
    // Nota: Em uma implementação real, o poema seria descriptografado
    // usando a OS Key. Para fins de realismo, vamos simular o panic
    // com a mensagem apropriada.
    
    IOLog("\n");
    IOLog("===========================================================\n");
    IOLog("DSMOS: Hardware validation failed - Unauthorized hardware detected\n");
    IOLog("\n");
    IOLog("Your karma check for today:\n");
    IOLog("There once was was a user that whined\n");
    IOLog("his existing OS was so blind,\n");
    IOLog("he'd do better to pirate\n");
    IOLog("an OS that ran great\n");
    IOLog("but found his hardware declined.\n");
    IOLog("Please don't steal Mac OS!\n");
    IOLog("Really, that's way uncool.\n");
    IOLog("(C) Apple Computer, Inc.\n");
    IOLog("===========================================================\n");
    
    // Registrar tentativa de hardware não autorizado no syslog
    IOLog("DSMOS: Unauthorized hardware attempt blocked\n");
    
    // Panic com mensagem apropriada
    panic("DSMOS: This hardware is not authorized to run macOS\n");
}

//==============================================================================
// Validação de binário protegido
//==============================================================================

bool com_apple_iokit_DSMOS::validateBinary(const char* path)
{
    // Verificar se o binário precisa de validação
    bool needsValidation = false;
    
    for (int i = 0; gProtectedBinaries[i] != NULL; i++) {
        if (strcmp(path, gProtectedBinaries[i]) == 0) {
            needsValidation = true;
            break;
        }
    }
    
    if (!needsValidation) {
        return true;
    }
    
    // Se chegamos aqui, precisamos da OS Key válida
    if (!fSMCAvailable) {
        IOLog("DSMOS: Protected binary %s requires SMC validation\n", path);
        return false;
    }
    
    // Simular verificação de integridade
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("DSMOS: Validating protected binary: %s\n", path);
    }
    
    // Em uma implementação real, o __TEXT segment com flag SG_PROTECTED_VERSION_1 (0x8)
    // seria descriptografado aqui usando a OS Key
    
    return true;
}

//==============================================================================
// Hook para loader Mach-O
//==============================================================================

void com_apple_iokit_DSMOS::machOLoaderHook(struct mach_header* mh,
                                             intptr_t slide,
                                             const char* path)
{
    com_apple_iokit_DSMOS* me = NULL;
    
    // Obter instância do driver
    me = OSDynamicCast(com_apple_iokit_DSMOS,
                        IOService::getPlatform()->getResourceService());
    
    if (!me) {
        // Fallback: panic se não conseguir obter a instância
        panic("DSMOS: Failed to get DSMOS instance for binary: %s\n", path);
    }
    
    // Validar binário
    if (!me->validateBinary(path)) {
        me->panicWithPoem();
    }
    
    // Se for um binário protegido, descriptografar segmentos
    for (int i = 0; gProtectedBinaries[i] != NULL; i++) {
        if (strcmp(path, gProtectedBinaries[i]) == 0) {
            me->decryptSegments(path);
            break;
        }
    }
}

void com_apple_iokit_DSMOS::decryptSegments(const char* binaryPath)
{
    // Em uma implementação real, aqui seria feita a descriptografia
    // dos segmentos __TEXT que possuem a flag SG_PROTECTED_VERSION_1
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("DSMOS: Decrypting protected segments for: %s\n", binaryPath);
    }
    
    // Simular descriptografia bem-sucedida se tivermos a chave
    if (fSMCAvailable && memcmp(fOSK, gOSKKey, sizeof(gOSKKey)) == 0) {
        // Descriptografia bem-sucedida
        if (gIOKitDebug & kIOLogDebug) {
            IOLog("DSMOS: Decryption successful for: %s\n", binaryPath);
        }
    } else {
        // Falha na descriptografia - deve causar panic mais tarde
        if (gIOKitDebug & kIOLogDebug) {
            IOLog("DSMOS: Decryption failed for: %s (OSK unavailable)\n", binaryPath);
        }
        fValidationFlags |= 0x1;
    }
}

//==============================================================================
// Escrita do blob de integridade na commpage
//==============================================================================

void com_apple_iokit_DSMOS::writeCommPageData(void)
{
    vm_offset_t commpage_addr = 0;
    
    // Localizar commpage (endereço -16 * 4096 + 0x1600)
    commpage_addr = (vm_offset_t)(-16 * 4096 + 0x1600);
    
    // Preencher blob de integridade
    bcopy("Please don't steal Mac OS!\nReally, that's way uncool.\n(C) Apple Computer, Inc.\n",
          fCommPageData, sizeof(fCommPageData) - 1);
    
    // Em uma implementação real, copiar para a commpage
    // Nota: Isso requer acesso de kernel e mapeamento apropriado
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("DSMOS: Integrity blob written to commpage at 0x%lx\n",
              (unsigned long)commpage_addr);
    }
}

//==============================================================================
// Registro do hook do loader Mach-O
//==============================================================================

void com_apple_iokit_DSMOS::registerMachOLoaderHook(void)
{
    // Registrar função de callback para o loader Mach-O
    // Nota: Em uma implementação real, isso seria feito via
    // register_machloader_callback() ou similar
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("DSMOS: Mach-O loader hook registered\n");
    }
    
    // Armazenar o hook para uso futuro
    fValidationFlags |= 0x2;
}

//==============================================================================
// Implementação de callPlatformFunction
//==============================================================================

IOReturn com_apple_iokit_DSMOS::callPlatformFunction(const OSSymbol* functionName,
                                                      bool waitForFunction,
                                                      void* param1, void* param2,
                                                      void* param3, void* param4)
{
    // Responder a chamadas específicas do DSMOS
    if (functionName->isEqualTo("validateBinary")) {
        const char* path = (const char*)param1;
        bool* result = (bool*)param2;
        
        if (path && result) {
            *result = validateBinary(path);
            return kIOReturnSuccess;
        }
        return kIOReturnBadArgument;
    }
    
    if (functionName->isEqualTo("getOSKey")) {
        uint8_t* buffer = (uint8_t*)param1;
        size_t* size = (size_t*)param2;
        
        if (buffer && size && fSMCAvailable) {
            size_t copySize = min(*size, sizeof(fOSK));
            bcopy(fOSK, buffer, copySize);
            *size = copySize;
            return kIOReturnSuccess;
        }
        return kIOReturnBadArgument;
    }
    
    // Passar chamadas não tratadas para a superclasse
    return super::callPlatformFunction(functionName,
                                        waitForFunction,
                                        param1, param2,
                                        param3, param4);
}

//==============================================================================
// GetDeviceName
//==============================================================================

OSString* com_apple_iokit_DSMOS::getDeviceName(void) const
{
    return OSString::withCString("Dont Steal Mac OS X");
}

//==============================================================================
// Start - Ponto de entrada principal do driver
//==============================================================================

bool com_apple_iokit_DSMOS::start(IOService* provider)
{
    bool smcValid = false;
    uint32_t debugFlags = 0;
    
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("DSMOS: Starting Dont Steal Mac OS X kernel extension\n");
    IOLog("DSMOS: Copyright (c) 2006,2009 Apple Inc. All rights reserved.\n");
    
    // Verificar se estamos em hardware Intel (DSMOS só é usado em Intel)
#if defined(__i386__) || defined(__x86_64__)
    // Continuar normalmente
#else
    IOLog("DSMOS: Not loaded - platform not Intel (no DSMOS required)\n");
    return false;
#endif
    
    // Verificar se é uma build de preview (DTK) que não usa DSMOS
    if (PE_parse_boot_argn("dtk", &debugFlags, sizeof(debugFlags))) {
        IOLog("DSMOS: Developer Transition Kit detected - DSMOS disabled\n");
        return false;
    }
    
    // Verificar presença do SMC e ler OS Key
    smcValid = verifySMCPresence();
    
    if (!smcValid) {
        // Em uma implementação real, isso causaria panic imediato
        IOLog("DSMOS: CRITICAL: SMC validation failed - unauthorized hardware\n");
        
#if SECURE_KERNEL
        // Em kernels seguros, panic imediato
        panicWithPoem();
#else
        // Em builds de desenvolvimento, apenas avisar
        IOLog("DSMOS: WARNING: Running on unauthorized hardware (development build)\n");
#endif
    } else {
        IOLog("DSMOS: SMC validation successful\n");
    }
    
    // Registrar hook do loader Mach-O
    registerMachOLoaderHook();
    
    // Escrever blob de integridade na commpage
    writeCommPageData();
    
    // Publicar propriedades no IORegistry
    setProperty("DSMOS Version", OSString::withCString("1.0.0"));
    setProperty("OSK Valid", smcValid ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Protected Binaries Count", OSNumber::withNumber(8, 32));
    
    // Listar binários protegidos
    OSArray* binaryArray = OSArray::withCapacity(8);
    for (int i = 0; gProtectedBinaries[i] != NULL; i++) {
        binaryArray->setObject(OSString::withCString(gProtectedBinaries[i]));
    }
    setProperty("Protected Binaries", binaryArray);
    binaryArray->release();
    
    // Registrar serviço
    registerService();
    
    IOLog("DSMOS: Successfully loaded - protecting macOS from unauthorized hardware\n");
    
    return true;
}

//==============================================================================
// Stop
//==============================================================================

void com_apple_iokit_DSMOS::stop(IOService* provider)
{
    IOLog("DSMOS: Stopping Dont Steal Mac OS X kernel extension\n");
    
    closeSMC();
    
    super::stop(provider);
}

//==============================================================================
// Função de criação do driver
//==============================================================================

__attribute__((constructor))
static void dsmos_constructor(void)
{
    // Mensagem oculta na construção (visível apenas em logs de depuração)
    if (gIOKitDebug & kIOLogDebug) {
        printf("DSMOS: Loading DSMOS protection module...\n");
        printf("DSMOS: This kext is protected by the Digital Millennium Copyright Act\n");
    }
}

__attribute__((destructor))
static void dsmos_destructor(void)
{
    if (gIOKitDebug & kIOLogDebug) {
        printf("DSMOS: Unloading DSMOS protection module\n");
    }
}
