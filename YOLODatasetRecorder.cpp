#include "YOLODatasetRecorder.h"

#include "YOLOAnnotationComponent.h"
#include "YOLOAnnotationTypes.h"

#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace YOLODatasetRecorderPrivate
{
    FString ResolveDatasetRoot(const FString& OutputFolder)
    {
        FString Folder = OutputFolder;

        if (Folder.IsEmpty())
        {
            Folder = TEXT("YOLODataset");
        }

        if (FPaths::IsRelative(Folder))
        {
            Folder = FPaths::Combine(
                FPaths::ProjectSavedDir(),
                Folder
            );
        }

        return FPaths::ConvertRelativePathToFull(Folder);
    }

    FString MakeSafeFrameName(const FString& RequestedName)
    {
        FString FrameName = RequestedName;

        if (FrameName.IsEmpty())
        {
            FrameName =
                FDateTime::Now().ToString(
                    TEXT("yyyyMMdd_HHmmss_fff")
                );
        }

        return FPaths::MakeValidFileName(FrameName);
    }

    /**
     * The boxes produced by UYOLOAnnotationComponent are in player-viewport
     * coordinates. A constrained Cine Camera may occupy only a centered
     * sub-rectangle of that viewport (letterbox/pillarbox).
     *
     * This reconstructs that exact image rectangle so YOLO normalization is
     * relative to the pixels that belong to the camera image, not the bars.
     */
    FIntRect CalculateCameraViewRect(
        const FMinimalViewInfo& ViewInfo,
        int32 ViewportWidth,
        int32 ViewportHeight
    )
    {
        int32 ViewMinX = 0;
        int32 ViewMinY = 0;
        int32 ViewWidth = ViewportWidth;
        int32 ViewHeight = ViewportHeight;

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
                // Viewport is wider than camera -> pillarbox.
                ViewWidth = FMath::Max(
                    1,
                    FMath::RoundToInt(
                        static_cast<double>(ViewportHeight) *
                        CameraAspect
                    )
                );

                ViewMinX = (ViewportWidth - ViewWidth) / 2;
            }
            else if (ViewportAspect < CameraAspect)
            {
                // Viewport is taller/narrower than camera -> letterbox.
                ViewHeight = FMath::Max(
                    1,
                    FMath::RoundToInt(
                        static_cast<double>(ViewportWidth) /
                        CameraAspect
                    )
                );

                ViewMinY = (ViewportHeight - ViewHeight) / 2;
            }
        }

        return FIntRect(
            ViewMinX,
            ViewMinY,
            ViewMinX + ViewWidth,
            ViewMinY + ViewHeight
        );
    }

    FString MakeSemanticClassKey(
        const FYOLOScreenBoundingBox& Box
    )
    {
        return Box.Label.IsNone()
            ? TEXT("Unlabeled")
            : Box.Label.ToString();
    }

    /**
     * Mixing semantic mode and instance-separated mode in the same classes.txt
     * would silently change the meaning of class IDs. Keep one mode per dataset.
     */
    bool EnsureDatasetMode(
        const FString& DatasetRoot,
        bool bSeparateInstancesByID
    )
    {
        const FString ModePath =
            FPaths::Combine(
                DatasetRoot,
                TEXT("label_mode.txt")
            );

        const FString RequestedMode =
            bSeparateInstancesByID
                ? TEXT("instance")
                : TEXT("semantic");

        if (FPaths::FileExists(ModePath))
        {
            FString ExistingMode;

            if (!FFileHelper::LoadFileToString(
                    ExistingMode,
                    *ModePath))
            {
                UE_LOG(
                    LogTemp,
                    Error,
                    TEXT(
                        "YOLO recorder could not read dataset mode file: %s"
                    ),
                    *ModePath
                );

                return false;
            }

            ExistingMode.TrimStartAndEndInline();

            if (!ExistingMode.Equals(
                    RequestedMode,
                    ESearchCase::IgnoreCase))
            {
                UE_LOG(
                    LogTemp,
                    Error,
                    TEXT(
                        "YOLO recorder refused to mix label modes in '%s'. "
                        "This dataset is '%s', but this capture requested '%s'. "
                        "Use a different OutputFolder when changing "
                        "bSeparateInstancesByID."
                    ),
                    *DatasetRoot,
                    *ExistingMode,
                    *RequestedMode
                );

                return false;
            }

            return true;
        }

        return FFileHelper::SaveStringToFile(
            RequestedMode + LINE_TERMINATOR,
            *ModePath
        );
    }

    bool LoadClasses(
        const FString& ClassesPath,
        TArray<FString>& OutClasses
    )
    {
        OutClasses.Reset();

        if (!FPaths::FileExists(ClassesPath))
        {
            return true;
        }

        if (!FFileHelper::LoadFileToStringArray(
                OutClasses,
                *ClassesPath))
        {
            return false;
        }

        for (FString& ClassName : OutClasses)
        {
            ClassName.TrimStartAndEndInline();
        }

        OutClasses.RemoveAll(
            [](const FString& ClassName)
            {
                return ClassName.IsEmpty();
            }
        );

        return true;
    }

    int32 FindOrAddClass(
        const FString& ClassKey,
        TArray<FString>& Classes,
        bool& bOutClassesChanged
    )
    {
        const int32 ExistingIndex =
            Classes.IndexOfByPredicate(
                [&ClassKey](const FString& Existing)
                {
                    return Existing.Equals(
                        ClassKey,
                        ESearchCase::CaseSensitive
                    );
                }
            );

        if (ExistingIndex != INDEX_NONE)
        {
            return ExistingIndex;
        }

        bOutClassesChanged = true;
        return Classes.Add(ClassKey);
    }

    bool SaveClasses(
        const FString& ClassesPath,
        const TArray<FString>& Classes
    )
    {
        FString Contents;

        for (const FString& ClassName : Classes)
        {
            // classes.txt follows the common YOLO convention:
            // line number == numeric class ID.
            Contents += ClassName;
            Contents += LINE_TERMINATOR;
        }

        return FFileHelper::SaveStringToFile(
            Contents,
            *ClassesPath
        );
    }

    bool UpdateInstanceMap(
        const FString& InstanceMapPath,
        const TArray<FYOLOScreenBoundingBox>& Boxes
    )
    {
        TMap<int32, FString> ExistingEntries;

        if (FPaths::FileExists(InstanceMapPath))
        {
            TArray<FString> ExistingLines;

            if (!FFileHelper::LoadFileToStringArray(
                    ExistingLines,
                    *InstanceMapPath))
            {
                return false;
            }

            for (const FString& Line : ExistingLines)
            {
                FString IDText;
                FString Remainder;

                if (Line.Split(
                        TEXT(" "),
                        &IDText,
                        &Remainder,
                        ESearchCase::CaseSensitive,
                        ESearchDir::FromStart))
                {
                    const int32 ID = FCString::Atoi(*IDText);

                    if (ID > 0)
                    {
                        ExistingEntries.Add(ID, Line);
                    }
                }
            }
        }

        for (const FYOLOScreenBoundingBox& Box : Boxes)
        {
            if (Box.InstanceID <= 0)
            {
                continue;
            }

            const FString Label = Box.Label.IsNone()
                ? TEXT("Unlabeled")
                : Box.Label.ToString();

            const FString ActorName = IsValid(Box.Actor)
                ? Box.Actor->GetName()
                : TEXT("UnknownActor");

            ExistingEntries.Add(
                Box.InstanceID,
                FString::Printf(
                    TEXT("%d %s %s"),
                    Box.InstanceID,
                    *Label,
                    *ActorName
                )
            );
        }

        TArray<int32> IDs;
        ExistingEntries.GetKeys(IDs);
        IDs.Sort();

        FString Contents;

        for (const int32 ID : IDs)
        {
            Contents += ExistingEntries[ID];
            Contents += LINE_TERMINATOR;
        }

        return FFileHelper::SaveStringToFile(
            Contents,
            *InstanceMapPath
        );
    }

    bool BuildYOLOLabelText(
        const TArray<FYOLOScreenBoundingBox>& Boxes,
        const FIntRect& CameraViewRect,
        bool bSeparateInstancesByID,
        TArray<FString>& InOutClasses,
        bool& bOutClassesChanged,
        FString& OutLabelText
    )
    {
        OutLabelText.Reset();

        const double ImageWidth =
            static_cast<double>(CameraViewRect.Width());

        const double ImageHeight =
            static_cast<double>(CameraViewRect.Height());

        if (ImageWidth <= 0.0 || ImageHeight <= 0.0)
        {
            return false;
        }

        for (const FYOLOScreenBoundingBox& Box : Boxes)
        {
            // FYOLOScreenBoundingBox uses the player viewport coordinate system.
            // Remove the constrained-camera rectangle's origin before normalizing.
            const double Left = FMath::Clamp(
                Box.Min.X -
                    static_cast<double>(CameraViewRect.Min.X),
                0.0,
                ImageWidth
            );

            const double Top = FMath::Clamp(
                Box.Min.Y -
                    static_cast<double>(CameraViewRect.Min.Y),
                0.0,
                ImageHeight
            );

            const double Right = FMath::Clamp(
                Box.Max.X -
                    static_cast<double>(CameraViewRect.Min.X),
                0.0,
                ImageWidth
            );

            const double Bottom = FMath::Clamp(
                Box.Max.Y -
                    static_cast<double>(CameraViewRect.Min.Y),
                0.0,
                ImageHeight
            );

            const double BoxWidth = Right - Left;
            const double BoxHeight = Bottom - Top;

            if (BoxWidth <= 0.0 || BoxHeight <= 0.0)
            {
                continue;
            }

            // Standard YOLO normalized representation.
            const double CenterX =
                ((Left + Right) * 0.5) / ImageWidth;

            const double CenterY =
                ((Top + Bottom) * 0.5) / ImageHeight;

            const double NormalizedWidth =
                BoxWidth / ImageWidth;

            const double NormalizedHeight =
                BoxHeight / ImageHeight;

            int32 OutputLabelID = INDEX_NONE;

            if (bSeparateInstancesByID)
            {
                // IMPORTANT FOR TRACKING:
                // Use the exact same stable ID that the annotation component
                // assigned to this actor and encoded into Custom Stencil.
                // Do NOT derive a new ID from which objects happen to be visible.
                OutputLabelID = Box.InstanceID;

                if (OutputLabelID <= 0)
                {
                    continue;
                }
            }
            else
            {
                // Standard semantic YOLO mode: all actors sharing the same
                // semantic label also share the same class ID.
                const FString ClassKey =
                    MakeSemanticClassKey(Box);

                OutputLabelID =
                    FindOrAddClass(
                        ClassKey,
                        InOutClasses,
                        bOutClassesChanged
                    );
            }

            OutLabelText += FString::Printf(
                TEXT("%d %.6f %.6f %.6f %.6f%s"),
                OutputLabelID,
                CenterX,
                CenterY,
                NormalizedWidth,
                NormalizedHeight,
                LINE_TERMINATOR
            );
        }

        return true;
    }

    /**
     * Save a clean RGB image from the same camera state.
     *
     * We intentionally use a separate transient SceneCapture instead of a
     * viewport screenshot so:
     *   - the debug HUD is not baked into the dataset image;
     *   - the image uses the same Cine Camera projection;
     *   - the image dimensions match the camera image rectangle used for YOLO
     *     normalization.
     */
    bool SaveCleanRGBImage(
        UYOLOAnnotationComponent* AnnotationComponent,
        const FMinimalViewInfo& ViewInfo,
        const FIntRect& CameraViewRect,
        const FString& ImagePath
    )
    {
        if (!AnnotationComponent)
        {
            return false;
        }

        AActor* CameraOwner =
            AnnotationComponent->GetOwner();

        if (!CameraOwner)
        {
            return false;
        }

        const int32 ImageWidth =
            CameraViewRect.Width();

        const int32 ImageHeight =
            CameraViewRect.Height();

        if (ImageWidth <= 0 || ImageHeight <= 0)
        {
            return false;
        }

        UTextureRenderTarget2D* RenderTarget =
            NewObject<UTextureRenderTarget2D>(
                GetTransientPackage(),
                NAME_None,
                RF_Transient
            );

        if (!RenderTarget)
        {
            return false;
        }

        RenderTarget->ClearColor =
            FLinearColor::Black;

        RenderTarget->bForceLinearGamma = false;

        RenderTarget->InitCustomFormat(
            ImageWidth,
            ImageHeight,
            PF_B8G8R8A8,
            false
        );

        RenderTarget->UpdateResourceImmediate(true);

        USceneCaptureComponent2D* Capture =
            NewObject<USceneCaptureComponent2D>(
                CameraOwner,
                NAME_None,
                RF_Transient
            );

        if (!Capture)
        {
            return false;
        }

        CameraOwner->AddInstanceComponent(Capture);

        if (USceneComponent* Root =
                CameraOwner->GetRootComponent())
        {
            Capture->SetupAttachment(Root);
        }

        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->bAlwaysPersistRenderingState = true;

        Capture->CaptureSource =
            ESceneCaptureSource::SCS_FinalColorLDR;

        Capture->TextureTarget = RenderTarget;
        Capture->ShowFlags.SetPostProcessing(true);

        Capture->RegisterComponent();

        // Copy location, rotation, FOV, post processing, etc.
        Capture->SetCameraView(ViewInfo);

        // Match the exact projection strategy used by our working annotation
        // capture. This prevents the previous Cine Camera FOV/aspect mismatch.
        if (ViewInfo.ProjectionMode ==
                ECameraProjectionMode::Perspective &&
            ViewInfo.AspectRatio > KINDA_SMALL_NUMBER)
        {
            Capture->bUseCustomProjectionMatrix = true;
            Capture->CustomProjectionMatrix =
                ViewInfo.CalculateProjectionMatrix();
        }
        else
        {
            Capture->bUseCustomProjectionMatrix = false;
        }

        Capture->CaptureScene();

        FTextureRenderTargetResource* Resource =
            RenderTarget->GameThread_GetRenderTargetResource();

        if (!Resource)
        {
            Capture->DestroyComponent();
            return false;
        }

        TArray<FColor> Pixels;
        FReadSurfaceDataFlags ReadFlags(RCM_UNorm);

        if (!Resource->ReadPixels(Pixels, ReadFlags))
        {
            Capture->DestroyComponent();
            return false;
        }

        if (Pixels.Num() != ImageWidth * ImageHeight)
        {
            Capture->DestroyComponent();
            return false;
        }

        const FImageView ImageView(
            Pixels.GetData(),
            ImageWidth,
            ImageHeight,
            EGammaSpace::sRGB
        );

        const bool bSaved =
            FImageUtils::SaveImageByExtension(
                *ImagePath,
                ImageView,
                100
            );

        Capture->DestroyComponent();

        return bSaved;
    }
}

bool UYOLODatasetRecorder::RecordBoundingBoxes(
    UObject* WorldContextObject,
    UYOLOAnnotationComponent* AnnotationComponent,
    const FString& FrameName,
    bool bSeparateInstancesByID,
    bool bCaptureImage,
    const FString& OutputFolder
)
{
    using namespace YOLODatasetRecorderPrivate;

    UWorld* World = nullptr;

    if (AnnotationComponent)
    {
        World = AnnotationComponent->GetWorld();
    }

    if (!World &&
        GEngine &&
        WorldContextObject)
    {
        World =
            GEngine->GetWorldFromContextObject(
                WorldContextObject,
                EGetWorldErrorMode::ReturnNull
            );
    }

    if (!World)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "could not resolve a UWorld."
            )
        );

        return false;
    }

    APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(
            World,
            0
        );

    if (!PlayerController)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "Player 0 was not found."
            )
        );

        return false;
    }

    // Blueprint callers may omit the component. In that case, use the same
    // active-view-target convention as the HUD.
    if (!AnnotationComponent)
    {
        AActor* ViewTarget =
            PlayerController->GetViewTarget();

        if (ViewTarget)
        {
            AnnotationComponent =
                ViewTarget->FindComponentByClass<
                    UYOLOAnnotationComponent
                >();
        }
    }

    if (!AnnotationComponent)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: no "
                "UYOLOAnnotationComponent was supplied "
                "or found on the active ViewTarget."
            )
        );

        return false;
    }

    AActor* CameraOwner =
        AnnotationComponent->GetOwner();

    if (!CameraOwner)
    {
        return false;
    }

    int32 ViewportWidth = 0;
    int32 ViewportHeight = 0;

    PlayerController->GetViewportSize(
        ViewportWidth,
        ViewportHeight
    );

    if (ViewportWidth <= 0 ||
        ViewportHeight <= 0)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "invalid viewport size."
            )
        );

        return false;
    }

    FMinimalViewInfo ViewInfo;
    CameraOwner->CalcCamera(0.0f, ViewInfo);

    const FIntRect CameraViewRect =
        CalculateCameraViewRect(
            ViewInfo,
            ViewportWidth,
            ViewportHeight
        );

    const FString DatasetRoot =
        ResolveDatasetRoot(OutputFolder);

    const FString LabelsDirectory =
        FPaths::Combine(
            DatasetRoot,
            TEXT("labels")
        );

    const FString ImagesDirectory =
        FPaths::Combine(
            DatasetRoot,
            TEXT("images")
        );

    // Creating labels/ recursively also creates DatasetRoot.
    if (!IFileManager::Get().MakeDirectory(
            *LabelsDirectory,
            true))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "could not create '%s'."
            ),
            *LabelsDirectory
        );

        return false;
    }

    if (bCaptureImage &&
        !IFileManager::Get().MakeDirectory(
            *ImagesDirectory,
            true))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "could not create '%s'."
            ),
            *ImagesDirectory
        );

        return false;
    }

    if (!EnsureDatasetMode(
            DatasetRoot,
            bSeparateInstancesByID))
    {
        return false;
    }

    const FString ClassesPath =
        FPaths::Combine(
            DatasetRoot,
            TEXT("classes.txt")
        );

    TArray<FString> Classes;

    // classes.txt is only meaningful in semantic mode. In instance mode,
    // the first value in each row is the annotation/stencil InstanceID itself.
    if (!bSeparateInstancesByID &&
        !LoadClasses(
            ClassesPath,
            Classes))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "could not read '%s'."
            ),
            *ClassesPath
        );

        return false;
    }

    bool bClassesChanged = false;
    FString LabelText;

    if (!BuildYOLOLabelText(
            AnnotationComponent->GetScreenBoundingBoxes(),
            CameraViewRect,
            bSeparateInstancesByID,
            Classes,
            bClassesChanged,
            LabelText))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed while "
                "building normalized YOLO coordinates."
            )
        );

        return false;
    }

    if (!bSeparateInstancesByID &&
        bClassesChanged &&
        !SaveClasses(
            ClassesPath,
            Classes))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "could not save '%s'."
            ),
            *ClassesPath
        );

        return false;
    }

    if (bSeparateInstancesByID)
    {
        const FString InstanceMapPath =
            FPaths::Combine(
                DatasetRoot,
                TEXT("instances.txt")
            );

        if (!UpdateInstanceMap(
                InstanceMapPath,
                AnnotationComponent->GetScreenBoundingBoxes()))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT(
                    "RecordBoundingBoxes failed: "
                    "could not update '%s'."
                ),
                *InstanceMapPath
            );

            return false;
        }
    }

    const FString SafeFrameName =
        MakeSafeFrameName(FrameName);

    const FString LabelPath =
        FPaths::Combine(
            LabelsDirectory,
            SafeFrameName + TEXT(".txt")
        );

    if (!FFileHelper::SaveStringToFile(
            LabelText,
            *LabelPath))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "RecordBoundingBoxes failed: "
                "could not save '%s'."
            ),
            *LabelPath
        );

        return false;
    }

    if (bCaptureImage)
    {
        const FString ImagePath =
            FPaths::Combine(
                ImagesDirectory,
                SafeFrameName + TEXT(".png")
            );

        if (!SaveCleanRGBImage(
                AnnotationComponent,
                ViewInfo,
                CameraViewRect,
                ImagePath))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT(
                    "RecordBoundingBoxes saved labels, "
                    "but failed to save paired image '%s'."
                ),
                *ImagePath
            );

            return false;
        }
    }

    UE_LOG(
        LogTemp,
        Log,
        TEXT(
            "Recorded YOLO frame '%s': %d visible object(s) -> %s"
        ),
        *SafeFrameName,
        AnnotationComponent->GetScreenBoundingBoxes().Num(),
        *DatasetRoot
    );

    return true;
}
