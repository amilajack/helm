// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowserSynth.h"
#include "BrowserControl.h"
#include "load_save.h"
#include <emscripten.h>
#include <set>
static void reportFile(const String& error) {
  EM_ASM({HelmBrowser.reportFile(UTF8ToString($0));},error.toRawUTF8());
}
bool helmValidatePatch(const var& patch) {
  if (!patch.isObject()) return false;
  const auto version=patch.getProperty("synth_version", "0.4.1").toString();
  const auto settings=LoadSave::compareVersionStrings(version,"0.4.1")<=0 ? patch : patch["settings"];
  auto* object=settings.getDynamicObject();
  if (!object) return false;
  const auto* mods=settings["modulations"].getArray();
  if (!mods || mods->size()>256) return false;
  auto controls=helmSynth->getControls();
  auto sources=helmSynth->getEngine()->getModulationSources();
  for (const auto& pair : object->getProperties()) {
    auto name=pair.name.toString().toStdString();
    if (!controls.count(name)) continue; // versioned legacy fields are migrated by LoadSave
    if (!pair.value.isDouble() && !pair.value.isInt() && !pair.value.isInt64() && !pair.value.isBool()) return false;
    const double value=pair.value;
    auto details=mopo::Parameters::getDetails(name);
    // Older patches are transformed by the existing migrator. Reject invalid
    // numbers and excessive magnitudes before any mutable state is touched.
    if (!std::isfinite(value) || std::abs(value)>1e6) return false;
    // Shipped 0.9 presets retain values migrated from older control ranges.
    // Preserve those sounds rather than clamping them to today's UI ranges.
    double minimum=details.min, maximum=details.max;
    if (name=="cutoff") minimum=0;
    if (name=="stutter_frequency") minimum=-7;
    if (name=="distortion_drive") maximum=72;
    if (name=="beats_per_minute" && LoadSave::compareVersionStrings(version,"0.8.6")<=0) maximum=999;
    if (value<minimum || value>maximum) return false;
  }
  std::set<std::pair<std::string,std::string>> pairs;
  for (const auto& mod : *mods) {
    if (!mod.isObject()) return false;
    auto source=mod["source"].toString().toStdString();
    auto destination=mod["destination"].toString().toStdString();
    const auto amountValue=mod["amount"];
    if (!amountValue.isDouble() && !amountValue.isInt() && !amountValue.isInt64()) return false;
    const double amount=amountValue;
    if (!sources.count(source) || (!controls.count(destination) && destination!="osc_mix" && destination!="filter_saturation") ||
        !std::isfinite(amount) || std::abs(amount)>1e6 || !pairs.emplace(source,destination).second) return false;
    const auto migrated=destination=="osc_mix" ? "osc_1_volume" : destination=="filter_saturation" ? "distortion_drive" : destination;
    if (!helmSynth->getEngine()->getModulationDestination(migrated,sources.at(source)->owner->isPolyphonic())) return false;
  }
  return true;
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_load_patch(const char* path) {
  if (!helmTryControl()) return -1;
  const bool ok=helmSynth->loadFromFile(File(String::fromUTF8(path)));
  helmSynth->controlTick();
  if (ok && helmSynth->editor) helmSynth->editor->externalPatchLoaded(File(String::fromUTF8(path)));
  helmReleaseControl();
  if (!ok) reportFile("Unable to load patch. Current patch retained.");
  return ok;
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_save_patch(const char* path) {
  if (!helmTryControl()) return -1;
  const bool ok=helmSynth->saveToFile(File(String::fromUTF8(path)));
  helmReleaseControl();return ok;
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_export_patch() {
  helmQueueControl([] {helmSynth->exportToFile();});
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_import_patch() {
  auto chooser=std::make_shared<FileChooser>("Import Patch",File("/user"),"*.helm");
  chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectFiles,
    [chooser](const FileChooser& result) {
      const auto file=result.getResult();
      if (file==File()) return;
      auto destination=LoadSave::getUserBankDirectory().getChildFile("Imported").getChildFile(file.getFileName());
      if (file.getSize()>1024*1024 || !helmValidatePatch(JSON::parse(file.loadFileAsString()))) {
        reportFile("Invalid patch. Current patch and saved files retained.");return;
      }
      destination.getParentDirectory().createDirectory();
      destination=destination.getNonexistentSibling();
      if (!file.copyFileTo(destination)) {reportFile("Could not save imported patch.");return;}
      helmSynth->loadFromFile(destination);
      helmSynth->editor->externalPatchLoaded(destination);
    });
}
// Import into a new bank, staged completely and renamed once validated. Existing
// banks are never partially overwritten; malformed paths cannot escape staging.
void LoadSave::importBank() {
  auto chooser=std::make_shared<FileChooser>("Import Bank",File("/user"),"*.helmbank");
  chooser->launchAsync(FileBrowserComponent::openMode|FileBrowserComponent::canSelectFiles,
    [chooser](const FileChooser& result) {
      if (result.getResult()==File()) return;
      const auto source=result.getResult();
      const auto stage=File("/user/.import-"+Uuid().toString());
      auto fail=[&](String message){stage.deleteRecursively();reportFile(message);};
      if (source.getSize()>64*1024*1024) {fail("Bank exceeds 64 MiB.");return;}
      ZipFile zip(source);int64 total=0;
      if (zip.getNumEntries()<1 || zip.getNumEntries()>4096) {fail("Invalid bank archive.");return;}
      std::set<String> names;
      for(int i=0;i<zip.getNumEntries();++i) {
        const auto* entry=zip.getEntry(i);const auto name=entry->filename;
        total+=entry->uncompressedSize;
        if (name.startsWithChar('/') || name.containsChar('\\') || name.containsChar(':') ||
            StringArray::fromTokens(name,"/","").contains("..") || entry->isSymbolicLink ||
            entry->uncompressedSize<0 || entry->uncompressedSize>1024*1024 || total>64*1024*1024 ||
            !names.insert(name).second) {fail("Unsafe bank archive. No files imported.");return;}
        if (name.endsWithChar('/')) continue;
        if (!name.endsWithIgnoreCase(".helm")) {fail("Bank contains a non-patch file.");return;}
        std::unique_ptr<InputStream> input(zip.createStreamForEntry(i));
        if (!input) {fail("Unable to read bank.");return;}
        MemoryBlock bytes;
        input->readIntoMemoryBlock(bytes,1024*1024+1);
        if (bytes.getSize()>1024*1024 || bytes.getSize()!=size_t(entry->uncompressedSize) || !input->isExhausted()) {
          fail("Bank entry size does not match its header.");return;
        }
        const auto text=String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize()));
        if (!helmValidatePatch(JSON::parse(text))) {fail("Bank contains an invalid patch.");return;}
        const auto parts=StringArray::fromTokens(name,"/","");
        if (parts.size()<2 || parts.size()>3) {fail("Bank must contain folders of patches.");return;}
        const auto relative=parts.size()==3 ? parts[1]+"/"+parts[2] : name;
        auto target=stage.getChildFile(relative);
        if (target.exists()) {fail("Bank contains duplicate patch names.");return;}
        target.getParentDirectory().createDirectory();
        if (!target.replaceWithText(text)) {fail("Unable to stage bank.");return;}
      }
      auto destination=LoadSave::getBankDirectory().getChildFile(source.getFileNameWithoutExtension()).getNonexistentSibling();
      if (!stage.moveFileTo(destination)) {fail("Unable to install bank.");return;}
      reportFile("Bank imported into User Patches.");
    });
}
extern "C" int helm_file_dialog_defer();
extern "C" void helm_file_dialog_deferred_done(int,int,const char*);
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_write_bank(int token,const char* bankName,const char* path) {
  Array<File> patches;
  auto bank=LoadSave::getBankDirectory().getChildFile(String::fromUTF8(bankName));
  bank.findChildFiles(patches,File::findFiles,true,"*.helm");
  ZipFile::Builder archive;
  for(const auto& patch:patches) archive.addFile(patch,2,patch.getRelativePathFrom(LoadSave::getBankDirectory()));
  bool ok=false;
  { // Close the stream before notifying JavaScript to read the finished archive.
    FileOutputStream output(File(String::fromUTF8(path)).withFileExtension("helmbank"));
    ok=!output.failedToOpen() && archive.writeToStream(output,nullptr);
    output.flush();
    ok=ok && output.getStatus().wasOk();
  }
  helm_file_dialog_deferred_done(token,ok,ok?"":"Bank export failed");
}
void LoadSave::exportBank(String bankName) {
  // Obtain the destination during the user's gesture, before any downloads.
  auto chooser=std::make_shared<FileChooser>("Export Bank",File("/user").getChildFile(bankName+".helmbank"),"*.helmbank");
  chooser->launchAsync(FileBrowserComponent::saveMode|FileBrowserComponent::canSelectFiles,
    [chooser,bankName](const FileChooser& result) {
      if(result.getResult()==File())return;
      const int token=helm_file_dialog_defer();
      const auto path=result.getResult().getFullPathName();
      EM_ASM({const token=$0;const bank=UTF8ToString($1);const path=UTF8ToString($2);
        HelmLibrary.prepareBank(bank).then(()=>HelmControl.run(()=>Module.ccall('helm_browser_write_bank',null,['number','string','string'],[token,bank,path])))
          .catch(error=>HelmBrowser.deferredDone(token,0,error.message));
      },token,bankName.toRawUTF8(),path.toRawUTF8());
    });
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_export_bank(const char* name) {
  const String bank(name);helmQueueControl([bank]{LoadSave::exportBank(bank);});
}
extern "C" EMSCRIPTEN_KEEPALIVE void helm_browser_import_bank() {
  LoadSave::importBank();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* helm_browser_controls() {
  static String result;
  Array<var> entries;
  for (const auto& control : helmSynth->getControls()) {
    const auto details=mopo::Parameters::getDetails(control.first);
    DynamicObject::Ptr entry=new DynamicObject();
    entry->setProperty("name",String(control.first));
    entry->setProperty("min",details.min);entry->setProperty("max",details.max);
    entry->setProperty("default",details.default_value);
    entries.add(var(entry.get()));
  }
  result=JSON::toString(var(entries));return result.toRawUTF8();
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_set_control(const char* key,double value) {
  if (!std::isfinite(value) || !helmTryControl()) return 0;
  const std::string name(key);
  auto controls=helmSynth->getControls();
  bool ok=controls.count(name)!=0;
  if (ok) {
    const auto details=mopo::Parameters::getDetails(name);
    ok=value>=details.min && value<=details.max;
    if (ok) {
      helmSynth->valueChangedInternal(name,value);
      helmSynth->controlTick();
      if (helmSynth->editor) helmSynth->editor->updateGuiControl(name,value);
    }
  }
  helmReleaseControl();return ok;
}
extern "C" EMSCRIPTEN_KEEPALIVE double helm_browser_get_control(const char* key) {
  const auto& controls=helmSynth->getControls();
  const auto found=controls.find(key);
  return found==controls.end() ? std::numeric_limits<double>::quiet_NaN() : found->second->value();
}
extern "C" EMSCRIPTEN_KEEPALIVE int helm_browser_modulation(const char* source,const char* destination,double amount) {
  if (!std::isfinite(amount) || !helmTryControl()) return 0;
  const auto sources=helmSynth->getEngine()->getModulationSources();
  const auto found=sources.find(source);
  const bool valid=found!=sources.end() && helmSynth->getEngine()->getModulationDestination(destination,found->second->owner->isPolyphonic());
  if (valid) {helmSynth->changeModulationAmount(source,destination,amount);helmSynth->controlTick();helmSynth->editor->updateFullGui();}
  helmReleaseControl();return valid;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* helm_browser_validate_file(const char* path) {
  const File file(String::fromUTF8(path));
  if (!file.hasFileExtension("helm")) return ""; // bank validation runs transactionally on import
  if (!helmTryControl()) return "Synth is busy; retry import";
  const bool ok=file.getSize()<=1024*1024 && helmValidatePatch(JSON::parse(file.loadFileAsString()));
  helmReleaseControl();return ok ? "" : "Invalid Helm patch";
}
