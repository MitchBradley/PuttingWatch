//! Pure putting-stroke logic, ported from lib/PuttingCore of the original C++ firmware (branch `old-cpp-version`).
//! No ESP or I/O dependencies, so it builds and tests on the host.

mod gravity;
pub mod portal;
mod swing;
pub mod update;
pub mod update_view;
mod vec3;

pub use gravity::GravityFilter;
pub use swing::{Config, Event, MotionSample, State, SwingPhase, SwingTracker};
pub use vec3::{angle_between_degrees, Vec3};
