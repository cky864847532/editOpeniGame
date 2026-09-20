#pragma once

#include "iGameFilter.h"

#include <string>

IGAME_NAMESPACE_BEGIN

/**
 * @brief 高程标量场过滤器（DIME #19）
 *
 * 将每个点沿方向向量 d 的投影长度 h = p·d 线性映射到输出区间
 * [Low, High]，生成点标量属性（默认名 "Elevation"）。
 *
 * - 方向向量无需归一化（公共缩放会在归一化中被约去）；
 * - 平面网格垂直于方向时（投影范围退化）降级输出常量 Low，不产生 NaN；
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

    // 设置输出区间；要求 low < high，非法输入被拒绝并保持原值
    void SetOutputRange(double low, double high);
    double GetLowValue() const { return m_Low; }
    double GetHighValue() const { return m_High; }

    void SetArrayName(const std::string& name);
    const std::string& GetArrayName() const { return m_ArrayName; }

    bool Execute() override;

protected:
    ElevationFilter();
    ~ElevationFilter() override = default;

private:
    Vector3f m_Direction{0.f, 0.f, 1.f};  // 投影方向，默认 +Z
    double m_Low{0.0};                    // 输出区间下限
    double m_High{1.0};                   // 输出区间上限
    std::string m_ArrayName{"Elevation"};
};

IGAME_NAMESPACE_END
