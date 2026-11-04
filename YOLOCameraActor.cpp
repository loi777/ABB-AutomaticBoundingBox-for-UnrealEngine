// Fill out your copyright notice in the Description page of Project Settings.

#include "YOLOCameraActor.h"

#include "YOLOAnnotationComponent.h"
#include "InputCoreTypes.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

AYOLOCameraActor::AYOLOCameraActor(
    const FObjectInitializer& ObjectInitializer
)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;

    AnnotationComponent = CreateDefaultSubobject<UYOLOAnnotationComponent>(
        TEXT("YOLOAnnotationComponent")
    );
}

void AYOLOCameraActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // The annotation component handles discovery/projection independently.
    // This actor only handles temporary camera controls while it is active.
    if (!IsActiveYOLOCamera())
    {
        return;
    }

    HandleTestLook();
    HandleTestMovement(DeltaTime);
}

void AYOLOCameraActor::RefreshTrackedActors()
{
    if (AnnotationComponent)
    {
        AnnotationComponent->RefreshTrackedActors();
    }
}

bool AYOLOCameraActor::IsActiveYOLOCamera() const
{
    const APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(GetWorld(), 0);

    return PlayerController && PlayerController->GetViewTarget() == this;
}

void AYOLOCameraActor::HandleTestMovement(float DeltaTime)
{
    APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(GetWorld(), 0);

    if (!PlayerController)
    {
        return;
    }

    FVector MovementDirection = FVector::ZeroVector;

    if (PlayerController->IsInputKeyDown(EKeys::W))
    {
        MovementDirection += GetActorForwardVector();
    }

    if (PlayerController->IsInputKeyDown(EKeys::S))
    {
        MovementDirection -= GetActorForwardVector();
    }

    if (PlayerController->IsInputKeyDown(EKeys::D))
    {
        MovementDirection += GetActorRightVector();
    }

    if (PlayerController->IsInputKeyDown(EKeys::A))
    {
        MovementDirection -= GetActorRightVector();
    }

    if (PlayerController->IsInputKeyDown(EKeys::E))
    {
        MovementDirection += FVector::UpVector;
    }

    if (PlayerController->IsInputKeyDown(EKeys::Q))
    {
        MovementDirection -= FVector::UpVector;
    }

    float CurrentSpeed = MovementSpeed;

    if (PlayerController->IsInputKeyDown(EKeys::LeftShift))
    {
        CurrentSpeed *= FastMovementMultiplier;
    }

    if (!MovementDirection.IsNearlyZero())
    {
        MovementDirection.Normalize();

        AddActorWorldOffset(
            MovementDirection * CurrentSpeed * DeltaTime
        );
    }
}

void AYOLOCameraActor::HandleTestLook()
{
    APlayerController* PlayerController =
        UGameplayStatics::GetPlayerController(GetWorld(), 0);

    if (!PlayerController)
    {
        return;
    }

    // Only rotate while holding right mouse button.
    if (!PlayerController->IsInputKeyDown(EKeys::RightMouseButton))
    {
        return;
    }

    float MouseX = 0.0f;
    float MouseY = 0.0f;

    PlayerController->GetInputMouseDelta(MouseX, MouseY);

    if (FMath::IsNearlyZero(MouseX) &&
        FMath::IsNearlyZero(MouseY))
    {
        return;
    }

    FRotator Rotation = GetActorRotation();

    // Horizontal mouse motion turns left/right.
    Rotation.Yaw += MouseX * MouseSensitivity;

    // Negative because moving the mouse upward should look upward.
    Rotation.Pitch += MouseY * MouseSensitivity;

    // Prevent the camera from flipping upside down.
    Rotation.Pitch = FMath::Clamp(
        Rotation.Pitch,
        -MaxPitchAngle,
        MaxPitchAngle
    );

    Rotation.Roll = 0.0f;

    SetActorRotation(Rotation);
}
