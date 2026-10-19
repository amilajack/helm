#include "src/browser/MidiQueue.h"
#include <cstdlib>
#include <vector>

int main() {
    Helm::Web::MidiQueue queue;
    std::vector<int> notes;
    auto deliver = [&](const auto& event, int) { notes.push_back(event.bytes[1]); };
    auto require = [](bool condition) { if (!condition) std::abort(); };
    require(queue.push(0x90, 60, 100, 0));
    queue.panic();
    require(queue.push(0x90, 64, 100, 0));
    require(queue.drain(0, 128, deliver));
    require(notes.empty());
    require(!queue.drain(128, 128, deliver));
    require(notes == std::vector<int>{64});
    notes.clear();
    queue.panic();
    require(queue.push(0x90, 65, 100, 0));
    queue.panic();
    require(queue.push(0x90, 67, 100, 0));
    require(queue.drain(256, 128, deliver));
    require(!queue.drain(384, 128, deliver));
    require(notes == std::vector<int>{67});
    for (unsigned i = 0; i < queue.capacity; ++i) require(queue.push(0x90, 60, 100, 0));
    require(!queue.push(0x90, 60, 100, 0));
    require(queue.drain(512, 128, deliver));
    require(queue.pending() == 0 && queue.overflowCount() == 1);
    require(queue.push(0x90, 69, 100, 0));
    require(!queue.drain(640, 128, deliver));
    require(notes == std::vector<int>({67, 69}));
}
