// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CineCameraActor.h"
#include "YOLOAnnotationComponent.h"
#include "YOLOCameraActor.generated.h"

/**
 * Cine camera used by the synthetic-data workflow.
 *
 * The camera itself is intentionally kept focused on camera behaviour.
 * Object discovery and bounding-box calculations live in
 * UYOLOAnnotationComponent.
 */
UCLASS()
class <YOURMODULE>_API AYOLOCameraActor : public ACineCameraActor
{
    GENERATED_BODY()

public:
    AYOLOCameraActor(const FObjectInitializer& ObjectInitializer);

    virtual void Tick(float DeltaTime) override;

    // Temporary free-camera controls for testing.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Movement")
    float MovementSpeed = 500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Movement")
    float FastMovementMultiplier = 4.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Movement")
    float MouseSensitivity = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "YOLO|Movement")
    float MaxPitchAngle = 89.0f;

    // Annotation work is delegated to this component.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "YOLO|Annotation")
    TObjectPtr<UYOLOAnnotationComponent> AnnotationComponent;

    // Convenience forwarding function for Blueprint/debug use.
    UFUNCTION(BlueprintCallable, Category = "YOLO|Annotation")
    void RefreshTrackedActors();

    // True only while this camera actor is Player 0's current view target.
    UFUNCTION(BlueprintPure, Category = "YOLO")
    bool IsActiveYOLOCamera() const;

private:
    void HandleTestMovement(float DeltaTime);
    void HandleTestLook();
};
