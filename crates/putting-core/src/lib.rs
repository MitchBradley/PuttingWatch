//! Pure putting-stroke logic, ported from PuttingWatch's lib/PuttingCore (C++).
//! No ESP or I/O dependencies, so it builds and tests on the host.

mod gravity;
mod swing;
mod vec3;

pub use gravity::GravityFilter;
pub use swing::{Config, Event, MotionSample, State, SwingPhase, SwingTracker};
pub use vec3::{angle_between_degrees, Vec3};
