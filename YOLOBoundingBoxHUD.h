#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "YOLOBoundingBoxHUD.generated.h"

/**
 * Display-only HUD for YOLO annotations.
 *
 * It does not calculate bounding boxes. It simply looks for a
 * UYOLOAnnotationComponent on the active view target and draws the boxes
 * already produced by that component.
 */
UCLASS()
class MEATFACTORY_API AYOLOBoundingBoxHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;

protected:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Display")
    FLinearColor BoxColor = FLinearColor::Green;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Display", meta = (ClampMin = "0.5"))
    float BoxThickness = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Display")
    bool bDrawLabels = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Display")
    FLinearColor LabelColor = FLinearColor::Green;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Display", meta = (ClampMin = "0.1"))
    float LabelScale = 1.0f;
};
