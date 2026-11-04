#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "YOLOAnnotationTypes.h"
#include "YOLOAnnotationComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

/**
 * Owns YOLO annotation discovery and visible-pixel bounding-box generation.
 *
 * Tracked actors are marked with unique Custom Stencil instance IDs. A hidden
 * SceneCapture2D renders a post-process object-ID mask from the active camera
 * view. The CPU then scans that mask and creates the smallest rectangle around
 * the pixels that are actually visible for each tracked actor.
 */
UCLASS(ClassGroup = (YOLO), meta = (BlueprintSpawnableComponent))
class MEATFACTORY_API UYOLOAnnotationComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UYOLOAnnotationComponent();

    virtual void BeginPlay() override;

    virtual void TickComponent(
        float DeltaTime,
        ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction
    ) override;

    // Re-scan the level for actors carrying BoundingBoxTag and assign stencil IDs.
    UFUNCTION(BlueprintCallable, Category = "YOLO|Annotation")
    void RefreshTrackedActors();

    const TArray<FYOLOScreenBoundingBox>& GetScreenBoundingBoxes() const
    {
        return ScreenBoundingBoxes;
    }

    // Actor tag used to opt an actor into annotation.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Annotation")
    FName BoundingBoxTag = FName(TEXT("BB"));

    // Post-process material that writes visible Custom Stencil IDs into the red channel.
    // Assign M_YOLO_ObjectID (created in the Material Editor) to this property.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Visible Pixels")
    TObjectPtr<UMaterialInterface> AnnotationPostProcessMaterial = nullptr;

    // 1.0 = one mask pixel per game-viewport pixel. Lower values are faster for previewing.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Visible Pixels",
        meta = (ClampMin = "0.1", ClampMax = "1.0"))
    float MaskResolutionScale = 1.0f;

    // Ignore extremely tiny/noisy detections. Keep this at 1 to label any visible pixel.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Visible Pixels",
        meta = (ClampMin = "1"))
    int32 MinimumVisiblePixels = 1;

protected:
    // True when this component's owning actor is Player 0's current view target.
    bool IsOwnerActiveViewTarget() const;

private:
    struct FTrackedYOLOActor
    {
        TWeakObjectPtr<AActor> Actor;
        uint8 InstanceID = 0;
        FName Label = NAME_None;
    };

    struct FPixelBoundsAccumulator
    {
        int32 MinX = MAX_int32;
        int32 MinY = MAX_int32;
        int32 MaxX = -1;
        int32 MaxY = -1;
        int32 PixelCount = 0;

        void AddPixel(int32 X, int32 Y)
        {
            MinX = FMath::Min(MinX, X);
            MinY = FMath::Min(MinY, Y);
            MaxX = FMath::Max(MaxX, X);
            MaxY = FMath::Max(MaxY, Y);
            ++PixelCount;
        }

        bool IsValid(int32 MinimumPixels) const
        {
            return PixelCount >= MinimumPixels && MaxX >= MinX && MaxY >= MinY;
        }
    };

    // Tracked actors and their 1..255 stencil instance IDs.
    TArray<FTrackedYOLOActor> TrackedActors;

    // Rebuilt from the object-ID mask while the owner is the active camera.
    UPROPERTY(BlueprintReadOnly, Transient, Category = "YOLO|Annotation", meta = (AllowPrivateAccess = "true"))
    TArray<FYOLOScreenBoundingBox> ScreenBoundingBoxes;

    // Hidden annotation capture resources.
    UPROPERTY(Transient)
    TObjectPtr<USceneCaptureComponent2D> AnnotationCapture = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> AnnotationRenderTarget = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> AnnotationMaterialInstance = nullptr;

    bool bWarnedMissingMaterial = false;

    FName GetActorLabel(AActor* Actor) const;

    void AssignStencilIDs();
    bool EnsureCaptureResources(int32 ViewportWidth, int32 ViewportHeight);
    void SyncCaptureToOwnerCamera(float DeltaTime);
    bool CaptureAndBuildVisibleBounds(int32 ViewportWidth, int32 ViewportHeight);
};
