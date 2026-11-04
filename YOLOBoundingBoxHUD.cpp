#include "YOLOBoundingBoxHUD.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "YOLOAnnotationComponent.h"
#include "YOLOAnnotationTypes.h"

namespace
{
    FLinearColor GetColorForInstanceID(int32 InstanceID)
    {
        // Spread neighbouring IDs around the hue wheel so consecutive objects
        // are easy to distinguish. ID 0 is reserved for background/fallback.
        if (InstanceID <= 0)
        {
            return FLinearColor::Green;
        }

        constexpr uint8 Saturation = 230;
        constexpr uint8 Value = 255;

        // 67 is coprime with 255, so IDs cycle through the hue range before
        // repeating. This gives deterministic colours for IDs 1..255.
        const uint8 Hue = static_cast<uint8>((InstanceID * 67) % 255);

        return FLinearColor::MakeFromHSV8(Hue, Saturation, Value);
    }

    FString GetDisplayText(const FYOLOScreenBoundingBox& Box)
    {
        if (!Box.Label.IsNone())
        {
            return FString::Printf(
                TEXT("%s [ID: %d]"),
                *Box.Label.ToString(),
                Box.InstanceID
            );
        }

        return FString::Printf(
            TEXT("[ID: %d]"),
            Box.InstanceID
        );
    }
}

void AYOLOBoundingBoxHUD::DrawHUD()
{
    Super::DrawHUD();

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    APlayerController* PlayerController = World->GetFirstPlayerController();
    if (!PlayerController)
    {
        return;
    }

    AActor* ViewTarget = PlayerController->GetViewTarget();
    if (!ViewTarget)
    {
        return;
    }

    // Keep the HUD independent from AYOLOCameraActor: any active view target
    // carrying a YOLO annotation component can provide boxes for display.
    UYOLOAnnotationComponent* AnnotationComponent =
        ViewTarget->FindComponentByClass<UYOLOAnnotationComponent>();

    if (!AnnotationComponent)
    {
        return;
    }

    for (const FYOLOScreenBoundingBox& Box :
         AnnotationComponent->GetScreenBoundingBoxes())
    {
        const float Left = static_cast<float>(Box.Min.X);
        const float Top = static_cast<float>(Box.Min.Y);
        const float Right = static_cast<float>(Box.Max.X);
        const float Bottom = static_cast<float>(Box.Max.Y);

        const FLinearColor InstanceColor =
            GetColorForInstanceID(Box.InstanceID);

        DrawLine(Left, Top, Right, Top, InstanceColor, BoxThickness);
        DrawLine(Right, Top, Right, Bottom, InstanceColor, BoxThickness);
        DrawLine(Right, Bottom, Left, Bottom, InstanceColor, BoxThickness);
        DrawLine(Left, Bottom, Left, Top, InstanceColor, BoxThickness);

        if (bDrawLabels)
        {
            const float LabelY = FMath::Max(0.0f, Top - 20.0f);
            const FString DisplayText = GetDisplayText(Box);

            // Use the same deterministic instance colour for the text and box
            // so it is easy to visually associate a label with its object.
            DrawText(
                DisplayText,
                InstanceColor,
                Left,
                LabelY,
                nullptr,
                LabelScale,
                false
            );
        }
    }
}
