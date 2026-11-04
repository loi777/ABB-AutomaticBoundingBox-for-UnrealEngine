#include "YOLOAnnotationComponent.h"

#include "Camera/CameraTypes.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UnrealClient.h"

UYOLOAnnotationComponent::UYOLOAnnotationComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;

    // The owner camera can move during its normal actor tick. Generate the
    // annotation mask afterwards so it uses the newest camera transform.
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UYOLOAnnotationComponent::BeginPlay()
{
    Super::BeginPlay();
    RefreshTrackedActors();
}

void UYOLOAnnotationComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction
)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    ScreenBoundingBoxes.Reset();

    // Only generate annotations while this component's owner is the active
    // Player 0 view target.
    if (!IsOwnerActiveViewTarget())
    {
        return;
    }

    APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(GetWorld(), 0);

    if (!PlayerController)
    {
        return;
    }

    int32 ViewportWidth = 0;
    int32 ViewportHeight = 0;
    PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);

    if (ViewportWidth <= 0 || ViewportHeight <= 0)
    {
        return;
    }

    if (!EnsureCaptureResources(ViewportWidth, ViewportHeight))
    {
        return;
    }

    SyncCaptureToOwnerCamera(DeltaTime);
    CaptureAndBuildVisibleBounds(ViewportWidth, ViewportHeight);
}

void UYOLOAnnotationComponent::RefreshTrackedActors()
{
    TrackedActors.Reset();
    ScreenBoundingBoxes.Reset();

    if (!GetWorld() || BoundingBoxTag.IsNone())
    {
        return;
    }

    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(
        GetWorld(),
        BoundingBoxTag,
        FoundActors
    );

    // Custom Stencil gives us 255 usable non-zero IDs.
    const int32 TrackCount = FMath::Min(FoundActors.Num(), 255);
    TrackedActors.Reserve(TrackCount);

    for (int32 Index = 0; Index < TrackCount; ++Index)
    {
        AActor* Actor = FoundActors[Index];
        if (!IsValid(Actor))
        {
            continue;
        }

        FTrackedYOLOActor& Tracked = TrackedActors.AddDefaulted_GetRef();
        Tracked.Actor = Actor;
        Tracked.InstanceID = static_cast<uint8>(Index + 1); // 0 = background
        Tracked.Label = GetActorLabel(Actor);
    }

    if (FoundActors.Num() > 255)
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("YOLOAnnotationComponent found %d BB actors, but only 255 stencil IDs are available. Only the first 255 will be tracked."),
            FoundActors.Num()
        );
    }

    AssignStencilIDs();

    UE_LOG(
        LogTemp,
        Log,
        TEXT("YOLOAnnotationComponent on '%s' is tracking %d actor(s) tagged '%s'."),
        *GetNameSafe(GetOwner()),
        TrackedActors.Num(),
        *BoundingBoxTag.ToString()
    );
}

bool UYOLOAnnotationComponent::IsOwnerActiveViewTarget() const
{
    if (!GetWorld() || !GetOwner())
    {
        return false;
    }

    const APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(GetWorld(), 0);

    return PlayerController && PlayerController->GetViewTarget() == GetOwner();
}

FName UYOLOAnnotationComponent::GetActorLabel(AActor* Actor) const
{
    if (!Actor)
    {
        return NAME_None;
    }

    // Existing convention: "BB" opts the actor in; the first other Actor Tag
    // is the semantic label.
    for (const FName& Tag : Actor->Tags)
    {
        if (Tag != BoundingBoxTag)
        {
            return Tag;
        }
    }

    return Actor->GetFName();
}

void UYOLOAnnotationComponent::AssignStencilIDs()
{
    for (const FTrackedYOLOActor& Tracked : TrackedActors)
    {
        AActor* Actor = Tracked.Actor.Get();
        if (!IsValid(Actor) || Tracked.InstanceID == 0)
        {
            continue;
        }

        TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
        Actor->GetComponents(PrimitiveComponents, true);

        for (UPrimitiveComponent* Primitive : PrimitiveComponents)
        {
            if (!IsValid(Primitive))
            {
                continue;
            }

            Primitive->SetRenderCustomDepth(true);
            Primitive->SetCustomDepthStencilValue(
                static_cast<int32>(Tracked.InstanceID)
            );
        }
    }
}

bool UYOLOAnnotationComponent::EnsureCaptureResources(
    int32 ViewportWidth,
    int32 ViewportHeight
)
{
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        return false;
    }

    if (!AnnotationPostProcessMaterial)
    {
        if (!bWarnedMissingMaterial)
        {
            bWarnedMissingMaterial = true;
            UE_LOG(
                LogTemp,
                Warning,
                TEXT("YOLOAnnotationComponent on '%s' has no AnnotationPostProcessMaterial. Assign M_YOLO_ObjectID in the component details."),
                *GetNameSafe(Owner)
            );
        }

        return false;
    }

    bWarnedMissingMaterial = false;

    if (!AnnotationCapture)
    {
        AnnotationCapture = NewObject<USceneCaptureComponent2D>(
            Owner,
            TEXT("YOLOAnnotationSceneCapture"),
            RF_Transient
        );

        if (!AnnotationCapture)
        {
            return false;
        }

        Owner->AddInstanceComponent(AnnotationCapture);

        if (USceneComponent* RootComponent = Owner->GetRootComponent())
        {
            AnnotationCapture->SetupAttachment(RootComponent);
        }

        // We trigger CaptureScene() ourselves after the owner camera has moved.
        AnnotationCapture->bCaptureEveryFrame = false;
        AnnotationCapture->bCaptureOnMovement = false;
        AnnotationCapture->bAlwaysPersistRenderingState = true;

        // FinalColorLDR is required so the post-process mask material is applied.
        AnnotationCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        AnnotationCapture->PostProcessBlendWeight = 1.0f;
        AnnotationCapture->ShowFlags.SetPostProcessing(true);

        AnnotationCapture->RegisterComponent();
    }

    if (!AnnotationMaterialInstance)
    {
        AnnotationMaterialInstance = UMaterialInstanceDynamic::Create(
            AnnotationPostProcessMaterial,
            this
        );

        if (!AnnotationMaterialInstance)
        {
            return false;
        }
    }

    // SetCameraView() may replace post-process settings later, so the blendable
    // is also re-added in SyncCaptureToOwnerCamera().
    AnnotationCapture->AddOrUpdateBlendable(
        AnnotationMaterialInstance,
        1.0f
    );

    const float SafeScale = FMath::Clamp(MaskResolutionScale, 0.1f, 1.0f);

    // Match the render target to the actual camera image rectangle rather than
    // blindly using the full PIE viewport. A constrained Cine Camera can render
    // into a letterboxed / pillarboxed sub-rectangle whose aspect ratio comes
    // from the camera filmback. If the mask uses the full viewport aspect while
    // the player camera uses the constrained aspect, the centre matches but the
    // error grows toward the edges.
    FMinimalViewInfo ViewInfo;
    Owner->CalcCamera(0.0f, ViewInfo);

    int32 CameraViewWidth = ViewportWidth;
    int32 CameraViewHeight = ViewportHeight;

    if (ViewInfo.bConstrainAspectRatio &&
        ViewInfo.AspectRatio > KINDA_SMALL_NUMBER)
    {
        const double ViewportAspect =
            static_cast<double>(ViewportWidth) /
            static_cast<double>(ViewportHeight);

        const double CameraAspect =
            static_cast<double>(ViewInfo.AspectRatio);

        if (ViewportAspect > CameraAspect)
        {
            // Viewport is wider than the camera: pillarbox left/right.
            CameraViewWidth = FMath::Max(
                1,
                FMath::RoundToInt(
                    static_cast<double>(ViewportHeight) * CameraAspect
                )
            );
        }
        else if (ViewportAspect < CameraAspect)
        {
            // Viewport is taller/narrower than the camera: letterbox top/bottom.
            CameraViewHeight = FMath::Max(
                1,
                FMath::RoundToInt(
                    static_cast<double>(ViewportWidth) / CameraAspect
                )
            );
        }
    }

    const int32 DesiredWidth = FMath::Clamp(
        FMath::RoundToInt(static_cast<float>(CameraViewWidth) * SafeScale),
        64,
        8192
    );

    const int32 DesiredHeight = FMath::Clamp(
        FMath::RoundToInt(static_cast<float>(CameraViewHeight) * SafeScale),
        64,
        8192
    );

    if (!AnnotationRenderTarget)
    {
        AnnotationRenderTarget = NewObject<UTextureRenderTarget2D>(
            this,
            TEXT("YOLOAnnotationRenderTarget"),
            RF_Transient
        );

        if (!AnnotationRenderTarget)
        {
            return false;
        }

        AnnotationRenderTarget->ClearColor = FLinearColor::Black;
        AnnotationRenderTarget->bForceLinearGamma = true;
    }

    if (AnnotationRenderTarget->SizeX != DesiredWidth ||
        AnnotationRenderTarget->SizeY != DesiredHeight)
    {
        AnnotationRenderTarget->InitCustomFormat(
            DesiredWidth,
            DesiredHeight,
            PF_B8G8R8A8,
            true
        );

        AnnotationRenderTarget->UpdateResourceImmediate(true);
    }

    AnnotationCapture->TextureTarget = AnnotationRenderTarget;
    return true;
}

void UYOLOAnnotationComponent::SyncCaptureToOwnerCamera(float DeltaTime)
{
    if (!AnnotationCapture || !GetOwner())
    {
        return;
    }

    FMinimalViewInfo ViewInfo;
    GetOwner()->CalcCamera(DeltaTime, ViewInfo);

    // Copy the active camera transform, FOV, aspect settings, post process, etc.
    AnnotationCapture->SetCameraView(ViewInfo);

    // SceneCapture2D otherwise rebuilds a projection for its own render target.
    // For a constrained Cine Camera that can differ from the projection used by
    // the player view even when FOVAngle looks identical.  Use the Cine Camera's
    // projection matrix explicitly so the mask and RGB image use the same lens.
    if (ViewInfo.ProjectionMode == ECameraProjectionMode::Perspective &&
        ViewInfo.AspectRatio > KINDA_SMALL_NUMBER)
    {
        AnnotationCapture->bUseCustomProjectionMatrix = true;
        AnnotationCapture->CustomProjectionMatrix =
            ViewInfo.CalculateProjectionMatrix();
    }
    else
    {
        AnnotationCapture->bUseCustomProjectionMatrix = false;
    }

    // IMPORTANT:
    // Do NOT reset PostProcessSettings here. In our UE 5.6 testing, clearing the
    // settings after SetCameraView() caused the manual SceneCapture to render
    // black. Keeping the camera settings and applying our post-process mask at
    // Scene Color After Tonemapping produces the expected render-target output.
    AnnotationCapture->PostProcessBlendWeight = 1.0f;
    AnnotationCapture->ShowFlags.SetPostProcessing(true);

    if (AnnotationMaterialInstance)
    {
        AnnotationCapture->AddOrUpdateBlendable(
            AnnotationMaterialInstance,
            1.0f
        );
    }
}

bool UYOLOAnnotationComponent::CaptureAndBuildVisibleBounds(
    int32 ViewportWidth,
    int32 ViewportHeight
)
{
    if (!AnnotationCapture ||
        !AnnotationRenderTarget ||
        !AnnotationMaterialInstance)
    {
        return false;
    }

    AnnotationCapture->CaptureScene();

    FTextureRenderTargetResource* RenderTargetResource =
        AnnotationRenderTarget->GameThread_GetRenderTargetResource();

    if (!RenderTargetResource)
    {
        return false;
    }

    TArray<FColor> Pixels;
    FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
    ReadFlags.SetLinearToGamma(false);

    if (!RenderTargetResource->ReadPixels(Pixels, ReadFlags))
    {
        return false;
    }

    const int32 MaskWidth = AnnotationRenderTarget->SizeX;
    const int32 MaskHeight = AnnotationRenderTarget->SizeY;

    if (MaskWidth <= 0 ||
        MaskHeight <= 0 ||
        Pixels.Num() != MaskWidth * MaskHeight)
    {
        return false;
    }

    // Direct indexing by 8-bit instance ID. ID 0 is background.
    TArray<FPixelBoundsAccumulator> BoundsByID;
    BoundsByID.SetNum(256);

    TArray<bool> IsTrackedID;
    IsTrackedID.Init(false, 256);

    for (const FTrackedYOLOActor& Tracked : TrackedActors)
    {
        if (Tracked.InstanceID != 0)
        {
            IsTrackedID[Tracked.InstanceID] = true;
        }
    }

    // Expected post-process output for a visible tracked pixel:
    //
    //   R = 1.0
    //   G = InstanceID / 255.0
    //   B = 1.0 - (InstanceID / 255.0)
    //
    // In our 8-bit render target that becomes approximately:
    //
    //   R = 255
    //   G = InstanceID
    //   B = 255 - InstanceID
    //
    // Background and occluded pixels must be (0,0,0).
    for (int32 Y = 0; Y < MaskHeight; ++Y)
    {
        const int32 RowOffset = Y * MaskWidth;

        for (int32 X = 0; X < MaskWidth; ++X)
        {
            const FColor& Pixel = Pixels[RowOffset + X];

            // This signature prevents ordinary scene RGB from being interpreted
            // as annotation data if the mask material is missing or broken.
            if (Pixel.R < 250)
            {
                continue;
            }

            const int32 GBTotal =
                static_cast<int32>(Pixel.G) +
                static_cast<int32>(Pixel.B);

            // Small tolerance for 8-bit quantization.
            if (FMath::Abs(GBTotal - 255) > 3)
            {
                continue;
            }

            const uint8 InstanceID = Pixel.G;

            if (InstanceID == 0 || !IsTrackedID[InstanceID])
            {
                continue;
            }

            BoundsByID[InstanceID].AddPixel(X, Y);
        }
    }

    // Convert mask pixels into the actual player camera image rectangle.
    // FOV is already copied by SetCameraView(); the missing piece was that a
    // constrained Cine Camera does not necessarily occupy the full viewport.
    FMinimalViewInfo ViewInfo;
    if (GetOwner())
    {
        GetOwner()->CalcCamera(0.0f, ViewInfo);
    }

    int32 CameraViewMinX = 0;
    int32 CameraViewMinY = 0;
    int32 CameraViewWidth = ViewportWidth;
    int32 CameraViewHeight = ViewportHeight;

    if (ViewInfo.bConstrainAspectRatio &&
        ViewInfo.AspectRatio > KINDA_SMALL_NUMBER)
    {
        const double ViewportAspect =
            static_cast<double>(ViewportWidth) /
            static_cast<double>(ViewportHeight);

        const double CameraAspect =
            static_cast<double>(ViewInfo.AspectRatio);

        if (ViewportAspect > CameraAspect)
        {
            CameraViewWidth = FMath::Max(
                1,
                FMath::RoundToInt(
                    static_cast<double>(ViewportHeight) * CameraAspect
                )
            );
            CameraViewMinX = (ViewportWidth - CameraViewWidth) / 2;
        }
        else if (ViewportAspect < CameraAspect)
        {
            CameraViewHeight = FMath::Max(
                1,
                FMath::RoundToInt(
                    static_cast<double>(ViewportWidth) / CameraAspect
                )
            );
            CameraViewMinY = (ViewportHeight - CameraViewHeight) / 2;
        }
    }

    const double ScaleX =
        static_cast<double>(CameraViewWidth) /
        static_cast<double>(MaskWidth);

    const double ScaleY =
        static_cast<double>(CameraViewHeight) /
        static_cast<double>(MaskHeight);

    // The mask coordinates above are expressed in the full viewport coordinate
    // system.  AHUD / player-relative screen coordinates can have a different
    // origin (the same issue previously fixed by ProjectWorldToScreen(..., true)).
    // Recover ONLY that origin translation here.  Scale/FOV are handled by the
    // exact capture projection and CameraViewWidth/Height above.
    FVector2D PlayerViewportOffset = FVector2D::ZeroVector;

    if (APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(GetWorld(), 0))
    {
        FVector CameraLocation;
        FRotator CameraRotation;
        PlayerController->GetPlayerViewPoint(CameraLocation, CameraRotation);

        const FVector TestPoint =
            CameraLocation + CameraRotation.Vector() * 1000.0;

        FVector2D AbsoluteScreenPosition;
        FVector2D RelativeScreenPosition;

        const bool bAbsoluteProjected =
            UGameplayStatics::ProjectWorldToScreen(
                PlayerController,
                TestPoint,
                AbsoluteScreenPosition,
                false
            );

        const bool bRelativeProjected =
            UGameplayStatics::ProjectWorldToScreen(
                PlayerController,
                TestPoint,
                RelativeScreenPosition,
                true
            );

        if (bAbsoluteProjected && bRelativeProjected)
        {
            PlayerViewportOffset =
                AbsoluteScreenPosition - RelativeScreenPosition;
        }
    }

    ScreenBoundingBoxes.Reserve(TrackedActors.Num());

    for (const FTrackedYOLOActor& Tracked : TrackedActors)
    {
        AActor* Actor = Tracked.Actor.Get();

        if (!IsValid(Actor) || Tracked.InstanceID == 0)
        {
            continue;
        }

        const FPixelBoundsAccumulator& PixelBounds =
            BoundsByID[Tracked.InstanceID];

        if (!PixelBounds.IsValid(FMath::Max(1, MinimumVisiblePixels)))
        {
            // Fully occluded or off-screen objects naturally have no visible
            // pixels and therefore produce no bounding box.
            continue;
        }

        FYOLOScreenBoundingBox& Box =
            ScreenBoundingBoxes.AddDefaulted_GetRef();

        Box.Actor = Actor;
        Box.Label = Tracked.Label;
        Box.InstanceID = static_cast<int32>(Tracked.InstanceID);
        Box.VisiblePixelCount = PixelBounds.PixelCount;

        Box.Min = FVector2D(
            static_cast<double>(CameraViewMinX) +
                static_cast<double>(PixelBounds.MinX) * ScaleX,
            static_cast<double>(CameraViewMinY) +
                static_cast<double>(PixelBounds.MinY) * ScaleY
        ) - PlayerViewportOffset;

        // MaxX/MaxY are inclusive pixel coordinates. +1 converts them to the
        // outer edge of the final visible pixel before mapping to viewport space.
        Box.Max = FVector2D(
            static_cast<double>(CameraViewMinX) +
                static_cast<double>(PixelBounds.MaxX + 1) * ScaleX,
            static_cast<double>(CameraViewMinY) +
                static_cast<double>(PixelBounds.MaxY + 1) * ScaleY
        ) - PlayerViewportOffset;

        // Keep HUD coordinates inside the current player viewport.
        Box.Min.X = FMath::Clamp(Box.Min.X, 0.0, static_cast<double>(ViewportWidth));
        Box.Min.Y = FMath::Clamp(Box.Min.Y, 0.0, static_cast<double>(ViewportHeight));
        Box.Max.X = FMath::Clamp(Box.Max.X, 0.0, static_cast<double>(ViewportWidth));
        Box.Max.Y = FMath::Clamp(Box.Max.Y, 0.0, static_cast<double>(ViewportHeight));
    }

    return true;
}
