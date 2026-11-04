#pragma once

#include "CoreMinimal.h"
#include "YOLOAnnotationTypes.generated.h"

/**
 * Screen-space annotation result for one tracked actor.
 *
 * For now, Min/Max come from projected geometric actor bounds.
 * Later the annotation component can fill the same structure from a
 * visible-pixel/object-ID mask without changing the camera or HUD API.
 */
USTRUCT(BlueprintType)
struct FYOLOScreenBoundingBox
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "YOLO")
    FVector2D Min = FVector2D::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "YOLO")
    FVector2D Max = FVector2D::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "YOLO")
    TObjectPtr<AActor> Actor = nullptr;

    UPROPERTY(BlueprintReadOnly, Category = "YOLO")
    FName Label = NAME_None;

    // Reserved for the later visible-pixel implementation.
    // Zero means that pixel visibility has not been calculated yet.
    UPROPERTY(BlueprintReadOnly, Category = "YOLO")
    int32 VisiblePixelCount = 0;

    // Reserved for the later object-ID / stencil implementation.
    // Zero means that no instance ID has been assigned yet.
    UPROPERTY(BlueprintReadOnly, Category = "YOLO")
    int32 InstanceID = 0;
};
