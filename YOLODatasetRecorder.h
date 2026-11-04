#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "YOLODatasetRecorder.generated.h"

class UYOLOAnnotationComponent;

/**
 * Reusable YOLO dataset writer.
 *
 * It does not own the camera and it does not calculate bounding boxes.
 * It consumes the already-calculated results from UYOLOAnnotationComponent.
 */
UCLASS()
class MEATFACTORY_API UYOLODatasetRecorder : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Save the current visible bounding boxes as a standard YOLO label file.
     *
     * Standard YOLO rows are:
     *
     *     <class_id> <x_center> <y_center> <width> <height>
     *
     * All four coordinates are normalized to 0..1 relative to the actual
     * Cine Camera image rectangle.
     *
     * @param WorldContextObject
     *     Any object belonging to the current world. Blueprint supplies this
     *     automatically because of the WorldContext metadata.
     *
     * @param AnnotationComponent
     *     Component providing the current FYOLOScreenBoundingBox array.
     *     May be null; in that case Player 0's current ViewTarget is searched
     *     for a UYOLOAnnotationComponent.
     *
     * @param FrameName
     *     Basename shared by label/image files, e.g. "frame_000123".
     *     If empty, a timestamp is generated.
     *
     * @param bSeparateInstancesByID
     *     false -> objects with the same semantic Label share one YOLO class.
     *              Example: every Pig is class "Pig".
     *
     *     true  -> the first value written to each row is the exact
     *              FYOLOScreenBoundingBox::InstanceID used by the
     *              Custom Stencil / post-process annotation mask.
     *              This keeps the same object ID across frames.
     *
     * @param bCaptureImage
     *     true -> also save a clean RGB PNG from the same camera state.
     *             The HUD bounding boxes are not drawn into this PNG.
     *
     * @param OutputFolder
     *     Relative paths are created under Project/Saved.
     *     Example: "YOLODataset" becomes:
     *
     *       Project/Saved/YOLODataset/
     *           classes.txt      (semantic mode)
     *           instances.txt    (instance-ID mode)
     *           label_mode.txt
     *           labels/
     *           images/
     *
     * @return true if every requested output was written successfully.
     */
    UFUNCTION(
        BlueprintCallable,
        Category = "YOLO|Recording",
        meta = (
            WorldContext = "WorldContextObject",
            DisplayName = "Record YOLO Bounding Boxes"
        )
    )
    static bool RecordBoundingBoxes(
        UObject* WorldContextObject,
        UYOLOAnnotationComponent* AnnotationComponent,
        const FString& FrameName,
        bool bSeparateInstancesByID,
        bool bCaptureImage,
        const FString& OutputFolder = TEXT("YOLODataset")
    );
};
