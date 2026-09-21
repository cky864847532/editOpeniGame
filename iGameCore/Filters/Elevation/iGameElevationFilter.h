#pragma once

#include "iGameFilter.h"

#include <string>

IGAME_NAMESPACE_BEGIN

/**
 * @brief 高程标量场过滤器（DIME #19，ParaView 兼容标尺语义）
 *
 * 将每个点沿方向向量 d 的投影长度 h = p·d̂（d̂ 为归一化方向）按固定
 * 标尺区间 [Low, High] 参数化为 t = (h − Low) / (High − Low)，
 * 并夹断到 [0, 1]，生成点标量属性（默认名 "Elevation"）。
 *
 * - 标尺语义与 ParaView / vtkElevationFilter 一致：标尺固定、不随数据
 *   自适应，移动/修改点位会改变输出值与着色（固定 range 改 point，
 *   颜色随之变化）；
 * - 取色范围锚定 [0, 1]：为 Elevation 属性挂显式 dataRange {0,1}，
 *   渲染/标量面板/色条按标尺着色，模型占标尺哪段颜色就占色带哪段；
 * - 低于标尺下限输出 0、高于上限输出 1（饱和），任何输入都不产生 NaN；
 * - 方向向量无需归一化（公共缩放会在归一化中被约去）；
 * - 独立输出：生成新的输出数据对象（几何与输入共享、属性集独立），
 *   Elevation 数组挂在输出对象的属性集上，输入对象保持原样，
 *   输出可在模型树中作为独立节点展示；重复执行时同名旧数组被覆盖。
 */
class ElevationFilter : public Filter {
public:
    I_OBJECT(ElevationFilter);
    static Pointer New() { return new ElevationFilter; }

    // 设置投影方向向量；零向量被拒绝并保持原方向
    bool SetDirection(float dx, float dy, float dz);
    bool SetDirection(const Vector3f& d);

    const Vector3f& GetDirection() const { return m_Direction; }

    // 设置投影标尺区间；要求 low < high，非法输入被拒绝并保持原值
    void SetRulerRange(double low, double high);
    double GetRulerLow() const { return m_Low; }
    double GetRulerHigh() const { return m_High; }

    void SetArrayName(const std::string& name);
    const std::string& GetArrayName() const { return m_ArrayName; }

    bool Execute() override;

protected:
    ElevationFilter();
    ~ElevationFilter() override = default;

private:
    Vector3f m_Direction{0.f, 0.f, 1.f};  // 投影方向，默认 +Z
    double m_Low{0.0};                    // 投影标尺区间下限
    double m_High{1.0};                   // 投影标尺区间上限
    std::string m_ArrayName{"Elevation"};
};

IGAME_NAMESPACE_END
