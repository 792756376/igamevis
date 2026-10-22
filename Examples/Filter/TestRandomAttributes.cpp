#include "DataProcessing/iGameRandomAttributesFilter.h"
#include "iGameFileIO.h"
#include "iGameInteractor.h"
#include "iGameRenderWindow.h"
#include "iGameScene.h"
#include "iGameUnstructuredMesh.h"
#include "iGameStructuredMesh.h"
#include "iGameAttributeSet.h"
#include "iGameType.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace iGame;


// ---------------------------------------------------------------------------
// TestRandomAttributes - 自动化测试（改进版，无需命令行参数）
//
// 改进点测试覆盖：
//   1. 独立输出：验证原模型属性不变
//   2. 二维结构网格 Cell 计数修正
//   3. 数组名称自定义 + 同名冲突处理（替换/改名/追加）
//   4. 输入校验（min > max 的错误提示）
//   5. 矢量生成（Generate Vectors）：固定 3 分量、元素数匹配
//   6. 标量/矢量同时生成（Both 模式）与自定义矢量名
//
// 测试流程（全自动）：
//   1) TetraMesh + IG_POINT → 验证独立输出原模型不变
//   2) TetraMesh + IG_CELL → 验证单元数匹配
//   3) StructuredGrid + IG_POINT → 验证点数
//   4) StructuredGrid + IG_CELL → 验证二维结构网格 Cell 计数
//   5) 同名冲突：AutoRename 模式测试
//   6) TetraMesh + Vector(点/单元) → 验证 3 分量与元素数
//   7) StructuredGrid + Both → 验证标量+矢量同时生成与自定义矢量名
// ---------------------------------------------------------------------------

static const char* kModel1 = "./Models/RandomAttributes_TetraMesh.vtk";
static const char* kModel2 = "./Models/RandomAttributes_StructuredGrid.vtk";

bool runSingleTest(const char* modelPath, IGenum attachType,
                   float minVal, float maxVal, unsigned seed,
                   const std::string& attrName = "",
                   RandomAttributesFilter::NameConflictMode mode = RandomAttributesFilter::NameConflictMode::Append) {
    std::string attachStr = (attachType == IG_CELL) ? "CELL" : "POINT";

    auto mesh = iGame::FileIO::ReadFile(modelPath);
    if (mesh == nullptr) {
        std::printf("[FAIL] %s: failed to read model\n", modelPath);
        return false;
    }
    auto attrSet = mesh->GetAttributeSet();
    if (attrSet == nullptr) {
        std::printf("[FAIL] %s: no attribute set\n", modelPath);
        return false;
    }
    size_t beforeAttrs = attrSet->GetNumberOfAttributes();

    auto ps = DynamicCast<iGame::PointSet>(mesh);
    auto stm = DynamicCast<iGame::StructuredMesh>(mesh);
    auto um = DynamicCast<iGame::UnstructuredMesh>(mesh);
    auto sm = DynamicCast<iGame::SurfaceMesh>(mesh);
    long long nPts = ps ? (long long)ps->GetNumberOfPoints() : -1;
    long long nCells = -1;
    if (stm) {
        // 结构化网格：单元数由结构尺寸隐式决定，与 RandomAttributesFilter::ComputeCount 保持一致
        // （StructuredMesh 继承 SurfaceMesh，若走 GetNumberOfFaces() 会取到面数而非体单元数，导致误判）
        int s[3] = {0, 0, 0};
        if (auto* sz = stm->GetDimensionSize()) { s[0] = sz[0]; s[1] = sz[1]; s[2] = sz[2]; }
        nCells = 1;
        if (s[0] > 1) nCells *= (s[0] - 1);
        if (s[1] > 1) nCells *= (s[1] - 1);
        if (s[2] > 1) nCells *= (s[2] - 1);
    } else if (um) {
        nCells = (long long)um->GetNumberOfCells();
    } else if (sm) {
        nCells = (long long)sm->GetNumberOfFaces();
    }
    std::printf("[INFO] model=%s  attach=%s  nPts=%lld  nCells=%lld\n",
                modelPath, attachStr.c_str(), nPts, nCells);

    auto filter = iGame::RandomAttributesFilter::New();
    filter->SetInput(mesh);
    filter->SetRange(minVal, maxVal);
    filter->SetSeed(seed);
    filter->SetAttachmentType(attachType);
    filter->SetNameConflictMode(mode);
    if (!attrName.empty()) filter->SetAttributeName(attrName);

    if (!filter->Execute()) {
        std::printf("[FAIL] %s: Execute() failed: %s\n",
                    modelPath, filter->GetMessage().c_str());
        return false;
    }

    // 改进点1：验证原模型属性不变
    size_t inputAttrs = mesh->GetAttributeSet()->GetNumberOfAttributes();
    if (inputAttrs != beforeAttrs) {
        std::printf("[FAIL] %s: input model was modified (before=%zu, after=%zu)\n",
                    modelPath, beforeAttrs, inputAttrs);
        return false;
    }

    // 验证输出属性
    auto output = filter->GetOutput();
    auto outAttrSet = output ? output->GetAttributeSet() : nullptr;
    if (!outAttrSet) {
        std::printf("[FAIL] %s: no output AttributeSet\n", modelPath);
        return false;
    }

    int outIdx = (int)outAttrSet->GetNumberOfAttributes() - 1;
    if (outIdx < 0) {
        std::printf("[FAIL] %s: no new attribute in output\n", modelPath);
        return false;
    }
    auto obj = outAttrSet->GetAttribute(outIdx);
    auto arr = DynamicCast<iGame::ArrayObject>(obj.pointer);
    if (!arr) {
        std::printf("[FAIL] %s: new attribute is not ArrayObject\n", modelPath);
        return false;
    }

    std::string outName = arr->GetName();
    IGsize attrCount = arr->GetNumberOfElements();
    long long expected = (attachType == IG_CELL) ? nCells : nPts;
    bool countOk = ((long long)attrCount == expected);

    std::printf("[PASS] %s: attach=%s  newAttr='%s'  nElem=%lld  expected=%lld  %s  inputUnchanged=YES\n",
                modelPath, attachStr.c_str(), outName.c_str(),
                (long long)attrCount, expected,
                countOk ? "COUNT_MATCH" : "COUNT_MISMATCH");

    return countOk;
}

// ---------------------------------------------------------------------------
// runVectorTest - 矢量生成测试（改进点5/6）
// kind 为 Vector 时验证 3 分量矢量；为 Both 时额外验证标量数组同时存在。
// ---------------------------------------------------------------------------
bool runVectorTest(const char* modelPath, IGenum attachType,
                   RandomAttributesFilter::GenerateKind kind,
                   float minVal, float maxVal, unsigned seed,
                   const std::string& vectorName = "") {
    const std::string attachStr = (attachType == IG_CELL) ? "CELL" : "POINT";
    const std::string kindStr =
            (kind == RandomAttributesFilter::GenerateKind::Both) ? "BOTH" : "VECTOR";

    auto mesh = iGame::FileIO::ReadFile(modelPath);
    if (mesh == nullptr) {
        std::printf("[FAIL] %s: failed to read model\n", modelPath);
        return false;
    }
    auto attrSet = mesh->GetAttributeSet();
    if (attrSet == nullptr) {
        std::printf("[FAIL] %s: no attribute set\n", modelPath);
        return false;
    }
    const size_t beforeAttrs = attrSet->GetNumberOfAttributes();

    long long nPts = -1, nCells = -1;
    {
        auto ps = DynamicCast<iGame::PointSet>(mesh);
        auto stm = DynamicCast<iGame::StructuredMesh>(mesh);
        auto um = DynamicCast<iGame::UnstructuredMesh>(mesh);
        auto sm = DynamicCast<iGame::SurfaceMesh>(mesh);
        nPts = ps ? (long long)ps->GetNumberOfPoints() : -1;
        if (stm) {
            int s[3] = {0, 0, 0};
            if (auto* sz = stm->GetDimensionSize()) { s[0] = sz[0]; s[1] = sz[1]; s[2] = sz[2]; }
            nCells = 1;
            if (s[0] > 1) nCells *= (s[0] - 1);
            if (s[1] > 1) nCells *= (s[1] - 1);
            if (s[2] > 1) nCells *= (s[2] - 1);
        } else if (um) {
            nCells = (long long)um->GetNumberOfCells();
        } else if (sm) {
            nCells = (long long)sm->GetNumberOfFaces();
        }
    }

    auto filter = iGame::RandomAttributesFilter::New();
    filter->SetInput(mesh);
    filter->SetRange(minVal, maxVal);
    filter->SetSeed(seed);
    filter->SetAttachmentType(attachType);
    filter->SetGenerateKind(kind);
    if (!vectorName.empty()) filter->SetVectorName(vectorName);

    if (!filter->Execute()) {
        std::printf("[FAIL] %s: Execute() failed: %s\n",
                    modelPath, filter->GetMessage().c_str());
        return false;
    }

    // 验证原模型属性不变
    if (mesh->GetAttributeSet()->GetNumberOfAttributes() != beforeAttrs) {
        std::printf("[FAIL] %s: input model was modified\n", modelPath);
        return false;
    }

    auto outAttrSet = filter->GetOutput() ? filter->GetOutput()->GetAttributeSet() : nullptr;
    if (!outAttrSet) {
        std::printf("[FAIL] %s: no output AttributeSet\n", modelPath);
        return false;
    }

    const char* vecExpectedName = vectorName.empty()
            ? (attachType == IG_CELL ? "RandomCellVectors" : "RandomPointVectors")
            : vectorName.c_str();

    // 按名称查找矢量数组（从后往前，兼容同名冲突改名后的情形）
    int vecIdx = -1;
    for (int i = (int)outAttrSet->GetNumberOfAttributes() - 1; i >= 0; --i) {
        auto& ref = outAttrSet->GetAttribute(i);
        if (!ref.pointer || ref.isDeleted) continue;
        if (ref.attachmentType != attachType) continue;
        if (ref.pointer->GetName() == std::string(vecExpectedName)) { vecIdx = i; break; }
    }
    if (vecIdx < 0) {
        std::printf("[FAIL] %s: vector array '%s' not found\n", modelPath, vecExpectedName);
        return false;
    }

    auto& vecAttr = outAttrSet->GetAttribute(vecIdx);
    auto vecArr = DynamicCast<iGame::ArrayObject>(vecAttr.pointer);
    if (!vecArr) {
        std::printf("[FAIL] %s: vector attribute is not ArrayObject\n", modelPath);
        return false;
    }
    const int dim = vecArr->GetDimension();
    const IGsize nElem = vecArr->GetNumberOfElements();
    const long long expected = (attachType == IG_CELL) ? nCells : nPts;
    const bool dimOk = (dim == 3);
    const bool countOk = ((long long)nElem == expected);
    const bool typeOk = (vecAttr.type == IG_VECTOR);

    std::printf("[%s] %s: kind=%s attach=%s vecName='%s' dim=%d nElem=%lld expected=%lld type=%s\n",
                (dimOk && countOk && typeOk) ? "PASS" : "FAIL", modelPath, kindStr.c_str(),
                attachStr.c_str(), vecArr->GetName().c_str(), dim, (long long)nElem, expected,
                typeOk ? "IG_VECTOR" : "OTHER");

    bool allOk = dimOk && countOk && typeOk;

    if (kind == RandomAttributesFilter::GenerateKind::Both) {
        // Both 模式还应同时存在标量数组（默认名）
        const char* scName = (attachType == IG_CELL) ? "RandomCellScalars" : "RandomPointScalars";
        int scIdx = -1;
        for (int i = (int)outAttrSet->GetNumberOfAttributes() - 1; i >= 0; --i) {
            auto& ref = outAttrSet->GetAttribute(i);
            if (!ref.pointer || ref.isDeleted) continue;
            if (ref.attachmentType != attachType) continue;
            if (ref.pointer->GetName() == std::string(scName)) { scIdx = i; break; }
        }
        auto scArr = scIdx >= 0
                ? DynamicCast<iGame::ArrayObject>(outAttrSet->GetAttribute(scIdx).pointer) : nullptr;
        const bool scOk = scArr && scArr->GetDimension() == 1 &&
                          (long long)scArr->GetNumberOfElements() == expected;
        std::printf("[%s] Both mode: scalar '%s' dim=%d nElem=%lld\n",
                    scOk ? "PASS" : "FAIL", scName,
                    scArr ? scArr->GetDimension() : -1,
                    scArr ? (long long)scArr->GetNumberOfElements() : -1);
        return allOk && scOk;
    }

    return allOk;
}


int main(int argc, char* argv[]) {
    bool noRender = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-render") == 0) noRender = true;
    }

    std::printf("============================================\n");
    std::printf("  TestRandomAttributes (Improved Auto Test)\n");
    std::printf("============================================\n\n");

    bool allPass = true;

    // Test 1: TetraMesh + IG_POINT (验证独立输出)
    std::printf("--- Test 1: TetraMesh + POINT (independent output) ---\n");
    allPass &= runSingleTest(kModel1, IG_POINT, 0.0f, 255.0f, 42u);

    // Test 2: TetraMesh + IG_CELL
    std::printf("\n--- Test 2: TetraMesh + CELL ---\n");
    allPass &= runSingleTest(kModel1, IG_CELL, 0.0f, 1.0f, 100u);

    // Test 3: StructuredGrid + IG_POINT
    std::printf("\n--- Test 3: StructuredGrid + POINT ---\n");
    allPass &= runSingleTest(kModel2, IG_POINT, -10.0f, 10.0f, 7u);

    // Test 4: StructuredGrid + IG_CELL (改进点4：二维结构网格 Cell 计数)
    std::printf("\n--- Test 4: StructuredGrid + CELL (2D cell count fix) ---\n");
    allPass &= runSingleTest(kModel2, IG_CELL, 0.0f, 1000.0f, 999u);

    // Test 5: 同名冲突 AutoRename 模式（先注入同名数组，确保真正触发自动改名）
    std::printf("\n--- Test 5: Name conflict AutoRename ---\n");
    {
        bool ok5 = true;
        auto mesh5 = iGame::FileIO::ReadFile(kModel1);
        if (!mesh5) {
            std::printf("[FAIL] failed to read %s\n", kModel1);
            ok5 = false;
        }

        // 第一步：Append 模式生成名为 RandomPointScalars 的数组
        auto f1 = iGame::RandomAttributesFilter::New();
        if (ok5) {
            f1->SetInput(mesh5);
            f1->SetRange(0.0f, 255.0f);
            f1->SetSeed(42u);
            f1->SetAttachmentType(IG_POINT);
            f1->SetAttributeName("RandomPointScalars");
            f1->SetNameConflictMode(RandomAttributesFilter::NameConflictMode::Append);
            ok5 = f1->Execute();
        }
        auto out1 = ok5 ? f1->GetOutput() : nullptr;

        // 第二步：AutoRename 模式应自动改名（RandomPointScalars_1），而非覆盖
        auto f2 = iGame::RandomAttributesFilter::New();
        if (out1) {
            f2->SetInput(out1);
            f2->SetRange(0.0f, 255.0f);
            f2->SetSeed(43u);
            f2->SetAttachmentType(IG_POINT);
            f2->SetAttributeName("RandomPointScalars");
            f2->SetNameConflictMode(RandomAttributesFilter::NameConflictMode::AutoRename);
            ok5 = f2->Execute();
        } else {
            ok5 = false;
        }
        auto out2 = ok5 ? f2->GetOutput() : nullptr;
        auto as2 = out2 ? out2->GetAttributeSet() : nullptr;

        if (as2 && as2->GetNumberOfAttributes() > 0) {
            size_t totalAttrs = as2->GetNumberOfAttributes();
            int lastIdx = (int)totalAttrs - 1;
            auto& lastAttr = as2->GetAttribute(lastIdx);
            auto lastArr = DynamicCast<iGame::ArrayObject>(lastAttr.pointer);
            std::string lastName = lastArr ? lastArr->GetName() : "";
            bool renamed = (lastName == "RandomPointScalars_1");
            std::printf("[%s] AutoRename: totalAttrs=%zu lastName='%s' (%s)\n",
                        renamed ? "PASS" : "FAIL",
                        totalAttrs, lastName.c_str(),
                        renamed ? "RENAMED" : "NOT_RENAMED");
            ok5 = renamed;
        } else {
            std::printf("[FAIL] AutoRename: no output attributes\n");
            ok5 = false;
        }
        allPass &= ok5;
    }

    // Test 6: 矢量生成（改进点5/6）：TetraMesh + Vector (POINT)
    std::printf("\n--- Test 6: TetraMesh + Vector (POINT) ---\n");
    allPass &= runVectorTest(kModel1, IG_POINT,
                             RandomAttributesFilter::GenerateKind::Vector,
                             0.0f, 10.0f, 5u);

    // Test 7: 矢量生成：TetraMesh + Vector (CELL)
    std::printf("\n--- Test 7: TetraMesh + Vector (CELL) ---\n");
    allPass &= runVectorTest(kModel1, IG_CELL,
                             RandomAttributesFilter::GenerateKind::Vector,
                             -5.0f, 5.0f, 6u);

    // Test 8: 标量 + 矢量同时生成（Both 模式）+ 自定义矢量名
    std::printf("\n--- Test 8: StructuredGrid + Both (POINT, custom vector name) ---\n");
    allPass &= runVectorTest(kModel2, IG_POINT,
                             RandomAttributesFilter::GenerateKind::Both,
                             0.0f, 100.0f, 8u, "MyRandomVectors");

    // Summary
    std::printf("\n============================================\n");
    std::printf("  Result: %s\n", allPass ? "ALL PASSED" : "SOME FAILED");
    std::printf("============================================\n");

    // --no-render mode: skip the render window for CTest/CI runs
    if (noRender) {
        return allPass ? 0 : 1;
    }

    // Render the last model with random scalars
    auto mesh = iGame::FileIO::ReadFile(kModel1);
    if (mesh) {
        // 展示矢量生成效果：按矢量的第 0 分量着色
        auto filter = iGame::RandomAttributesFilter::New();
        filter->SetInput(mesh);
        filter->SetRange(0.0f, 255.0f);
        filter->SetSeed(7u);
        filter->SetAttachmentType(IG_POINT);
        filter->SetGenerateKind(RandomAttributesFilter::GenerateKind::Vector);
        filter->Execute();

        auto output = filter->GetOutput();
        auto scene = iGame::Scene::New();
        scene->AddModel(output);
        auto drawObj = DynamicCast<iGame::DrawObject>(output);
        auto attrSet = output->GetAttributeSet();
        if (drawObj && attrSet) {
            int idx = (int)attrSet->GetNumberOfAttributes() - 1;
            if (idx >= 0) drawObj->ViewCloudPicture(scene, idx, 0);
        }

        auto window = iGame::RenderWindow::New();
        window->SetSize(960, 720);
        window->SetScene(scene);

        auto interactor = iGame::Interactor::New();
        interactor->Initialize(scene);
        interactor->CreateDefaultStyle();
        window->SetInteractor(interactor);
        window->Show();
    }

    return allPass ? 0 : 1;
}