
#include "../src/auto_quest_start.cpp"
#include <cassert>
#include <vector>
void WriteIni(const char* content){FILE* f=nullptr;assert(!fopen_s(&f,(DllDir()+"\\auto_quest_start.ini").c_str(),"wb"));fputs(content,f);fclose(f);}
void WP(uintptr_t a,uintptr_t p){memcpy((void*)a,&p,8);}
void NativeSites(){
 memcpy((void*)(g_exeBase+kOnePhaseJump),kOnePhaseExpected,2);
 memcpy((void*)(g_exeBase+kQuestId1),kQuestIdExpected,4);memcpy((void*)(g_exeBase+kQuestId2),kQuestIdExpected,4);
 memcpy((void*)(g_exeBase+kDlcInitCall),kDlcInitExpected,5);
}
void CheckSites(bool redirect,bool dlcPatched){
 assert(!memcmp((void*)(g_exeBase+kOnePhaseJump),redirect?kOnePhasePatched:kOnePhaseExpected,2));
 for(auto site:{kQuestId1,kQuestId2})assert(Rd32(g_exeBase+site)==(redirect?(uint32_t)g_cfg.quest_id:1u));
 assert(!memcmp((void*)(g_exeBase+kDlcInitCall),dlcPatched?kDlcInitPatched:kDlcInitExpected,5));
}
void FullReset(bool rehook){
 NativeSites();g_cfg={};g_cfg.quest_id=12345;g_cfg.rehook_on_return=rehook;
 g_titleContinued=false;g_startupLogoDone=true;g_returnTitlePolicyApplied=false;g_returnTitleRestoreWarning=false;
 assert(PatchAll(g_cfg,(HMODULE)g_exeBase));CheckSites(true,true);
}
int main(){
 // Missing new keys retain the previous complete-auto behavior.
 WriteIni("[auto_quest_start]\nenabled=1\nsave_slot=1\nquest_id=12345\n");
 auto cfg=LoadConfig();assert(cfg.enabled&&!cfg.title_screen_only&&cfg.rehook_on_return&&cfg.save_slot==1&&cfg.quest_id==12345);
 for(int only=0;only<2;++only)for(int rehook=0;rehook<2;++rehook){
  char ini[240];sprintf_s(ini,"\xEF\xBB\xBF[auto_quest_start]\nenabled=1\ntitle_screen_only=%d\nrehook_on_return=%d\nsave_slot=0\nquest_id=66845\n",only,rehook);
  WriteIni(ini);cfg=LoadConfig();assert(cfg.title_screen_only==(only!=0)&&cfg.rehook_on_return==(rehook!=0));
 }
 WriteIni("[auto_quest_start]\nenabled=0\ntitle_screen_only=1\n");assert(!LoadConfig().enabled);
 auto image=(unsigned char*)VirtualAlloc(nullptr,0x51C5000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(image);g_exeBase=(uintptr_t)image;
 unsigned char title[0x400]{},logo[0x200]{};std::vector<unsigned char> user(0x22000),saves(kSaveSlotSize*3);
 auto t=(uintptr_t)title,u=(uintptr_t)user.data();WP(g_exeBase+kPSave,u);WP(u+0xA8,(uintptr_t)saves.data());saves[0x38]=1;
 hook::Ctx c{};c.rcx=t;
 // Do not undo the first automatic entry while the original title is preparing.
 FullReset(false);OnTitlePrepare(&c);CheckSites(true,true);assert(!g_returnTitlePolicyApplied);
 // Restoring on PREPARE supports address reuse and happens before manual save choice.
 g_titleContinued=true;OnTitlePrepare(&c);CheckSites(false,false);assert(g_returnTitlePolicyApplied);
 FullReset(true);g_titleContinued=true;OnTitlePrepare(&c);CheckSites(true,false);
 // A soft return detected from execute is covered even without PREPARE.
 FullReset(false);g_titleContinued=true;c.rax=0;OnTitleStage(&c);CheckSites(false,false);assert(c.rax==0);
 // Unknown edits cause an all-or-nothing refusal; no unrelated patch is overwritten.
 FullReset(false);g_titleContinued=true;*(unsigned char*)(g_exeBase+kOnePhaseJump)=0xCC;
 unsigned char before[0x1A0];memcpy(before,(void*)(g_exeBase+0x25FD80),sizeof(before));
 ApplyReturnTitlePolicy();assert(!g_returnTitlePolicyApplied&&!memcmp(before,(void*)(g_exeBase+0x25FD80),sizeof(before)));
 // Title-only ignores quest and DLC patches, and dominates rehook_on_return.
 NativeSites();g_cfg={};g_cfg.title_screen_only=true;g_cfg.rehook_on_return=true;g_titleContinued=false;g_startupLogoDone=false;
 assert(PatchAll(g_cfg,(HMODULE)g_exeBase));CheckSites(false,false);
 g_cfg.enabled=false;assert(PatchAll(g_cfg,(HMODULE)g_exeBase));CheckSites(false,false);g_cfg.enabled=true;
 // No prefetch request, resource cut, save selection or quest automation occurs.
 Wr32((uintptr_t)logo+0x50,0x17);float timer=.5f;memcpy(logo+0x1C8,&timer,4);c={};c.rcx=(uintptr_t)logo;
 OnLogoExecute(&c);assert(Rd32((uintptr_t)logo+0x1C8)==0&&!g_titlePrefetch.attempted);
 g_trimCreditsPrefetch=true;assert(!IsInitialCreditsTrimTitle(t));assert(!ShouldOmitStartupScene(&c));g_trimCreditsPrefetch=false;
 Wr32(u+0xEC,2);user[0x524+8]=0xAA;
 Wr32(t+0x74,4);Wr32(t+0xB4,1);Wr32(u+0x1F980+0x50,6);
 timer=1.5f;memcpy((void*)(u+0x1F980+8),&timer,4);c={};c.rcx=t;c.rax=4;
 OnTitleStage(&c);assert(Rd32(u+0x1F980+8)==0&&Rd32(t+0x74)==4&&c.rax==4);
 assert(Rd32(u+0xEC)==2&&user[0x524+8]==0xAA&&!g_dlcEnumerated&&!g_titleContinued);
 Wr32(t+0xB4,2);OnTitleStage(&c);assert(c.rax==4&&Rd32(t+0x74)==4&&Rd32(u+0xEC)==2);
 CheckSites(false,false);assert(g_startupProfileFlushed);
 // Install the actual title-only hook set in a private fake module. Optional
 // scene/credits CALL sites and quest/DLC instructions must remain unmodified.
 auto dos=(IMAGE_DOS_HEADER*)image;dos->e_lfanew=0x80;
 auto nt=(IMAGE_NT_HEADERS64*)(image+0x80);nt->OptionalHeader.SizeOfImage=0x51C5000;
 const unsigned char logoPrepare[]={0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20};
 const unsigned char titlePrepare[]={0x48,0x89,0x5C,0x24,0x20,0x55,0x57,0x41,0x57};
 memcpy(image+0x24C4E0,logoPrepare,sizeof(logoPrepare));memcpy(image+0x25DA80,titlePrepare,sizeof(titlePrepare));
 memcpy(image+kLogoExecute,kLogoExecuteExpected,sizeof(kLogoExecuteExpected));
 memcpy(image+kLogoStart,kLogoStartExpected,sizeof(kLogoStartExpected));memcpy(image+kTitleStage,kStageExpected,sizeof(kStageExpected));
 memcpy(image+kTitleSceneRequest,kTitleSceneRequestExpected,5);
 const unsigned char submit[]={0xE8,0x3D,0x2F,0x92,0x01},create[]={0xE8,0xFD,0x12,0x82,0};
 memcpy(image+0x25DD2E,submit,5);memcpy(image+0x25D67E,create,5);
 assert(InstallHookOnce((HMODULE)image));
 assert(g_titleHookInstalled&&g_logoHookInstalled&&g_logoExecuteHookInstalled);
 assert(!g_sceneRequestHookInstalled&&!g_sceneRequestHookTried&&!g_creditsGatesTried&&!g_trimCreditsPrefetch);
 assert(!memcmp(image+kTitleSceneRequest,kTitleSceneRequestExpected,5)&&!memcmp(image+0x25DD2E,submit,5)&&!memcmp(image+0x25D67E,create,5));
 CheckSites(false,false);
 VirtualFree(image,0,MEM_RELEASE);
 puts("PASS: actual INI parsing/defaults, first entry retained, both return policies, soft re-entry, unknown-edit refusal.");
 puts("PASS: title-only timers without resource/selection/quest changes; real optional hooks absent; disabled behavior and mode precedence.");
}
