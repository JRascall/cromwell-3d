#include "render/overlay/BlastFlashes.hpp"

#include "render/style/Palette.hpp"

namespace xcom {

void BlastFlashes::add(float x, float height, float y)
{
    if (static_cast<int>(flashes_.size()) >= kMaxFlashes) return;
    flashes_.push_back({ x, y, height, 0.0f });
}

void BlastFlashes::update(float deltaSeconds)
{
    for (std::size_t i = 0; i < flashes_.size();) {
        flashes_[i].age += deltaSeconds;
        if (flashes_[i].age >= kLifetime) {
            flashes_[i] = flashes_.back();
            flashes_.pop_back();
        } else {
            i++;
        }
    }
}

void BlastFlashes::draw() const
{
    for (const Flash& flash : flashes_) {
        const float t = flash.age / kLifetime;
        Color colour = palette::kBlastFlash;
        colour.a = static_cast<unsigned char>(220 * (1.0f - t));
        DrawSphere(Vector3{ flash.x, flash.height, flash.y },
                   0.5f * (1.0f + t * 3.2f), colour);
    }
}

}  // namespace xcom
